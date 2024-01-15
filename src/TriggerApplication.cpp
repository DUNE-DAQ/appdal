/**
 * @file generate_modules.cpp
 *
 * Implementation of TriggerApplication's generate_modules dal method
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2023.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "ModuleFactory.hpp"

#include "oksdbinterfaces/Configuration.hpp"
#include "oks/kernel.hpp"

#include "coredal/Connection.hpp"
#include "coredal/DROStreamConf.hpp"
#include "coredal/NetworkConnection.hpp"
#include "coredal/ReadoutGroup.hpp"
#include "coredal/ResourceSet.hpp"
#include "coredal/Service.hpp"
#include "coredal/Session.hpp"

#include "appdal/NetworkConnectionRule.hpp"
#include "appdal/NetworkConnectionDescriptor.hpp"
#include "appdal/QueueConnectionRule.hpp"
#include "appdal/QueueDescriptor.hpp"

#include "appdal/LatencyBuffer.hpp"
#include "appdal/RequestHandler.hpp"

#include "appdal/ModuleLevelTriggerConf.hpp"
#include "appdal/ModuleLevelTrigger.hpp"

#include "appdal/TCSetTee.hpp"
#include "appdal/TCBufferConf.hpp"
#include "appdal/TCBuffer.hpp"

#include "appdal/RandomTriggerCandidateMakerConf.hpp"
#include "appdal/RandomTriggerCandidateMaker.hpp"
#include "appdal/TPChannelFilterConf.hpp"
#include "appdal/TriggeringAlgorithms.hpp"

#include "appdal/TriggerApplication.hpp"

#include "appdal/appdalIssues.hpp"

#include "logging/Logging.hpp"

#include <string>
#include <vector>

using namespace dunedaq;
using namespace dunedaq::appdal;

static ModuleFactory::Registrator
__reg__("TriggerApplication", [] (const SmartDaqApplication* smartApp,
                                  oksdbinterfaces::Configuration* confdb,
                                  const std::string& dbfile,
                                  const coredal::Session* session) -> ModuleFactory::ReturnType
  {
    auto app = smartApp->cast<TriggerApplication>();
    return app->generate_modules(confdb, dbfile, session);
  });

/**
 * \brief Helper function that gets a network connection config
 *
 * \param idname Unique ID name of the config object
 * \param ntDesc Network connection descriptor object
 * \param confdb Global database configuration
 * \param dbfile Database file location
 *
 * \ret OKS configuration object for the network connection
 */
oksdbinterfaces::ConfigObject 
create_network_connection(const std::string& idname,
                          const NetworkConnectionDescriptor* ntDesc,
                          oksdbinterfaces::Configuration* confdb,
                          const std::string& dbfile)
{
  auto ntServiceObj = ntDesc->get_associated_service()->config_object();
  std::string ntUid(idname);
  oksdbinterfaces::ConfigObject ntObj;
  confdb->create(dbfile, "NetworkConnection", ntUid, ntObj);
  ntObj.set_by_val<std::string>("connection_type", ntDesc->get_connection_type());
  ntObj.set_obj("associated_service", &ntServiceObj);

  return ntObj;
}

std::vector<const coredal::DaqModule*> 
TriggerApplication::generate_modules(oksdbinterfaces::Configuration* confdb,
                                     const std::string& dbfile,
                                     const coredal::Session* session) const 
{
  std::vector<const coredal::DaqModule*> modules;

  // Trigger Interrupt network descriptor
  const NetworkConnectionDescriptor* tiMLTNetDesc = nullptr;
  // TriggerDecisions network descriptor
  const NetworkConnectionDescriptor* tdMLTNetDesc = nullptr;
  // Timing trigger network descriptor
  const NetworkConnectionDescriptor* tmgTrgNetDesc = nullptr;

  // TC queue descriptor for link to the MLT
  //const QueueDescriptor* tcMLTQueueDesc;
  //const QueueDescriptor* rtcmQueueDesc;
  const QueueDescriptor* tcQueueDesc;

  /**************************************************************
   * Get all the queue connection descriptions
   **************************************************************/
  for (auto rule : get_queue_rules()) {
    std::string destination_class = rule->get_destination_class();
    std::string data_type = rule->get_descriptor()->get_data_type();
    if((destination_class == "ModuleLevelTrigger") && (data_type == "TriggerCandidate"))
      tcQueueDesc = rule->get_descriptor();
  }

  /**************************************************************
   * Get all the network connections
   **************************************************************/
  size_t nReadoutLinks = 0;
  for(auto rule : get_network_rules()){
    std::string endpoint_class = rule->get_endpoint_class();
    std::string data_type = rule->get_descriptor()->get_data_type();

    // Network connections for the MLT
    if(endpoint_class == "ModuleLevelTrigger" && data_type == "TriggerInhibit")
      tiMLTNetDesc = rule->get_descriptor();
    if(endpoint_class == "ModuleLevelTrigger" && data_type == "TriggerDecision")
      tdMLTNetDesc = rule->get_descriptor();
    if(endpoint_class == "TimingTriggerCandidateMaker" && data_type == "TriggerCandidate")
      tmgTrgNetDesc = rule->get_descriptor();

    std::cout << "Endpoint class: " << endpoint_class << " data_type: " << data_type << std::endl;
  }

  if(!tdMLTNetDesc)
    throw (BadConf(ERS_HERE, "No MLT network connection for the output TriggerDecision given"));

  /**************************************************************
   * Get the MLT
   **************************************************************/
  auto mlt_conf = get_mlt_conf();
  // Don't build the rest if we have no MLT
  if(!mlt_conf)
    return modules;

  // Vector of MLT inputs and output connections
  std::vector<oksdbinterfaces::ConfigObject> mlt_inputs_copies;
  std::vector<const oksdbinterfaces::ConfigObject*> mlt_inputs;
  std::vector<const oksdbinterfaces::ConfigObject*> mlt_outputs;

  // Create MLT config object
  auto mlt_conf_obj = mlt_conf->config_object();
  oksdbinterfaces::ConfigObject mltObj;
  std::string mltUid("mlt");
  confdb->create(dbfile, "ModuleLevelTrigger", mltUid, mltObj);
  mltObj.set_obj("configuration", &mlt_conf_obj);

  // Network connection for the MLT: input TriggerInhibit
  oksdbinterfaces::ConfigObject tiMLTNetObj = create_network_connection(std::string("df_busy_signal-") + UID(), tiMLTNetDesc, confdb, dbfile);
  mlt_inputs.push_back(&tiMLTNetObj);
  // Network connection for the MLT: output TriggerDecision
  oksdbinterfaces::ConfigObject tdMLTNetObj = create_network_connection(std::string("td_to_dfo-") + UID(), tdMLTNetDesc, confdb, dbfile);
  mlt_outputs.push_back(&tdMLTNetObj);

  /**************************************************************
   * Get the random trigger candidate makers
   **************************************************************/
  auto rndTCMakerConf  = get_random_candidate_maker_conf();
  if(rndTCMakerConf){
    if(!tcQueueDesc)
      throw (BadConf(ERS_HERE, "No RTCM queue description given"));

    std::cout << "Making the RTCM!" << std::endl;
    // Make RTCM object
    auto rndTCMakerConfObj = rndTCMakerConf->config_object();
    oksdbinterfaces::ConfigObject rndTCMakerObj;
    std::string rndTCMakerUid("rtcm-");
    confdb->create(dbfile, "RandomTriggerCandidateMaker", rndTCMakerUid + UID(), rndTCMakerObj);
    rndTCMakerObj.set_obj("configuration", &rndTCMakerConfObj);

    // Create the queue to the tee
    std::string rtcmToTeeQueueUid("rtcm-to-tee-");
    oksdbinterfaces::ConfigObject queueRTCMTeeObj;
    confdb->create(dbfile, "Queue", rtcmToTeeQueueUid + UID(), queueRTCMTeeObj);
    queueRTCMTeeObj.set_by_val<std::string>("data_type", tcQueueDesc->get_data_type());
    queueRTCMTeeObj.set_by_val<std::string>("queue_type", tcQueueDesc->get_queue_type());
    queueRTCMTeeObj.set_by_val<uint32_t>("capacity", tcQueueDesc->get_capacity());
    rndTCMakerObj.set_objs("outputs", {&queueRTCMTeeObj});

    // Create the tee itself
    std::string rtcmTCTeeUid("rtcm-TCSetTee-");
    oksdbinterfaces::ConfigObject tcTeeRTCMObj;
    confdb->create(dbfile, "TCSetTee", rtcmTCTeeUid + UID(), tcTeeRTCMObj);
    tcTeeRTCMObj.set_objs("inputs", {&queueRTCMTeeObj});

    // Queue to the buffer
    std::string rtcmTeeToBufferQueueUid("rtcm-tctee-to-buffer-");
    oksdbinterfaces::ConfigObject queueRTCMTeeToBufferObj;
    confdb->create(dbfile, "Queue", rtcmTeeToBufferQueueUid + UID(), queueRTCMTeeToBufferObj);
    queueRTCMTeeToBufferObj.set_by_val<std::string>("data_type", tcQueueDesc->get_data_type());
    queueRTCMTeeToBufferObj.set_by_val<std::string>("queue_type", tcQueueDesc->get_queue_type());
    queueRTCMTeeToBufferObj.set_by_val<uint32_t>("capacity", tcQueueDesc->get_capacity());

    // Queue to the MLT
    std::string rtcmTeeToMLTQueueUid("rtcm-tctee-to-mlt-");
    oksdbinterfaces::ConfigObject queueRTCMTeeToMLTObj;
    confdb->create(dbfile, "Queue", rtcmTeeToMLTQueueUid + UID(), queueRTCMTeeToMLTObj);
    queueRTCMTeeToMLTObj.set_by_val<std::string>("data_type", tcQueueDesc->get_data_type());
    queueRTCMTeeToMLTObj.set_by_val<std::string>("queue_type", tcQueueDesc->get_queue_type());
    queueRTCMTeeToMLTObj.set_by_val<uint32_t>("capacity", tcQueueDesc->get_capacity());

    // The TCBuffer
    std::string rtcmBufferUid("rtcm-tcbuffer");
    oksdbinterfaces::ConfigObject tcBufferRTCMObj;
    confdb->create(dbfile, "TCBuffer", rtcmBufferUid + UID(), tcBufferRTCMObj);

    auto tcBufferConf       = rndTCMakerConf->get_tcbuffer_conf();
    auto tc_buffer_conf_obj = tcBufferConf->config_object();
    //auto tcBufferLatency    = tcBufferConf->get_latencybuffer();
    //auto tcBufferReqHandler = tcBufferConf->get_requesthandler();

    tcBufferRTCMObj.set_obj("configuration", &tc_buffer_conf_obj);

    tcBufferRTCMObj.set_objs("inputs", {&queueRTCMTeeToBufferObj});

    modules.push_back(confdb->get<TCBuffer>(rtcmBufferUid + UID()));

    tcTeeRTCMObj.set_objs("outputs", {&queueRTCMTeeToBufferObj, &queueRTCMTeeToMLTObj});
    mlt_inputs.push_back(std::move(&queueRTCMTeeToMLTObj));

    // Push all the modules related with the RTCM 
    modules.push_back(confdb->get<RandomTriggerCandidateMaker>(rndTCMakerUid + UID()));
    modules.push_back(confdb->get<TCSetTee>(rtcmTCTeeUid + UID()));
  }

  /**************************************************************
   * Get the timing trigger candidate makers
   **************************************************************/
  auto tmgTCMakerConf = get_timing_candidate_maker_conf();
  if(tmgTCMakerConf){
    if(!tmgTrgNetDesc)
      throw (BadConf(ERS_HERE, "No timing trigger input network connection given"));

    // TODO: Should probably template this at some point...
    // Create the TimingTriggerCandidateMaker object
    std::cout << "Making the timing trigger!" << std::endl;
    auto tmgTCMakerConfObj = tmgTCMakerConf->config_object();
    oksdbinterfaces::ConfigObject tmgTCMakerObj;
    std::string tmgTCMakerUid("timing-");
    confdb->create(dbfile, "TimingTriggerCandidateMaker", tmgTCMakerUid + UID(), tmgTCMakerObj);
    tmgTCMakerObj.set_obj("configuration", &tmgTCMakerConfObj);

    // Create the network connection with trigger input
    oksdbinterfaces::ConfigObject tmgNetObj = create_network_connection(std::string("hsievents-") + UID(), tmgTrgNetDesc, confdb, dbfile);
    tmgTCMakerObj.set_objs("inputs", {&tmgNetObj});

    // Create queue to the tee
    // TODO: Once again, abstract into function
    std::string tmgToTeeQueueUid("tmg-to-tee-");
    oksdbinterfaces::ConfigObject queueTmgToTeeObj;
    confdb->create(dbfile, "Queue", tmgToTeeQueueUid + UID(), queueTmgToTeeObj);
    queueTmgToTeeObj.set_by_val<std::string>("data_type", tcQueueDesc->get_data_type());
    queueTmgToTeeObj.set_by_val<std::string>("queue_type", tcQueueDesc->get_queue_type());
    queueTmgToTeeObj.set_by_val<uint32_t>("capacity", tcQueueDesc->get_capacity());
    tmgTCMakerObj.set_objs("outputs", {&queueTmgToTeeObj});

    // Create the tee itself
    std::string tmgTCTeeUid("tmg-TCSetTee-");
    oksdbinterfaces::ConfigObject tcTeeTmgObj;
    confdb->create(dbfile, "TCSetTee", tmgTCTeeUid + UID(), tcTeeTmgObj);
    tcTeeTmgObj.set_objs("inputs", {&queueTmgToTeeObj});

    // Queue to the buffer
    std::string tmgTeeToBufferQueueUid("tmg-tctee-to-buffer-");
    oksdbinterfaces::ConfigObject queueTmgTeeToBufferObj;
    confdb->create(dbfile, "Queue", tmgTeeToBufferQueueUid + UID(), queueTmgTeeToBufferObj);
    queueTmgTeeToBufferObj.set_by_val<std::string>("data_type", tcQueueDesc->get_data_type());
    queueTmgTeeToBufferObj.set_by_val<std::string>("queue_type", tcQueueDesc->get_queue_type());
    queueTmgTeeToBufferObj.set_by_val<uint32_t>("capacity", tcQueueDesc->get_capacity());

    // Queue to the MLT
    // TODO: This is going to be wrong for now!
    // TODO: I think there can only be ONE TC queue to MLT, and that queue will have multiple inputs.
    // TODO: Should define that queue right at the top with the MLT.
    std::string tmgTeeToMLTQueueUid("tmg-tctee-to-mlt-");
    oksdbinterfaces::ConfigObject queueTmgTeeToMLTObj;
    confdb->create(dbfile, "Queue", tmgTeeToMLTQueueUid + UID(), queueTmgTeeToMLTObj);
    queueTmgTeeToMLTObj.set_by_val<std::string>("data_type", tcQueueDesc->get_data_type());
    queueTmgTeeToMLTObj.set_by_val<std::string>("queue_type", tcQueueDesc->get_queue_type());
    queueTmgTeeToMLTObj.set_by_val<uint32_t>("capacity", tcQueueDesc->get_capacity());

    // The TCBuffer
    std::string tmgBufferUid("tmg-tcbuffer");
    oksdbinterfaces::ConfigObject tcBufferTmgObj;
    confdb->create(dbfile, "TCBuffer", tmgBufferUid + UID(), tcBufferTmgObj);

    // TODO: This is very bad, TimingTimingCandidate should either have it's own buffer schema relation, or the conf should be taken from the application-buffer relationship (if any?)
    auto tcBufferConf       = rndTCMakerConf->get_tcbuffer_conf();
    auto tc_buffer_conf_obj = tcBufferConf->config_object();
    //auto tcBufferLatency    = tcBufferConf->get_latencybuffer();
    //auto tcBufferReqHandler = tcBufferConf->get_requesthandler();

    tcBufferTmgObj.set_obj("configuration", &tc_buffer_conf_obj);

    tcBufferTmgObj.set_objs("inputs", {&queueTmgTeeToBufferObj});

    // Push everything to modules & MLT queues
    modules.push_back(confdb->get<TCBuffer>(tmgBufferUid + UID()));

    tcTeeTmgObj.set_objs("outputs", {&queueTmgTeeToBufferObj, &queueTmgTeeToMLTObj});
    mlt_inputs.push_back(std::move(&queueTmgTeeToMLTObj));

    // Push all the modules related with the RTCM
    modules.push_back(confdb->get<TimingTriggerCandidateMaker>(tmgTCMakerUid + UID()));
    modules.push_back(confdb->get<TCSetTee>(tmgTCTeeUid + UID()));
  }

  // TODO: Warning! Not finished yet, remove and move down.
  mltObj.set_objs("inputs", mlt_inputs);
  mltObj.set_objs("outputs", mlt_outputs);

  modules.push_back(confdb->get<ModuleLevelTrigger>(mltUid));

  /**************************************************************
   * Get the trigger graph
   **************************************************************/
  // Don't bother building the entire trigger graph if we have no tpstreams in.
  if(!nReadoutLinks)
    return modules;

  // Get the triggering algos handler
  auto trigger_algs = get_trigger_algs();
  // Don't build the rest if we don't have triggering algs
  if(trigger_algs.size() == 0)
    return modules;

  // Make the TPChannelFilters
  auto tpChannelFilterConf = get_tpchannelfilter_conf();
  for(size_t tpLinkId = 0; tpLinkId < nReadoutLinks; ++tpLinkId){
    auto tpChannelFilterConfObj = tpChannelFilterConf->config_object();
  }

  // <Trigger algorithms code goes here>

  return modules;
}
