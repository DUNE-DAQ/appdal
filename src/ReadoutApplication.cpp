/**
 * @file generate_modules.cpp
 *
 * Implementation of ReadoutApplication's generate_modules dal method
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
#include "coredal/GeoId.hpp"
#include "coredal/NetworkConnection.hpp"
#include "coredal/ReadoutGroup.hpp"
#include "coredal/ReadoutInterface.hpp"
#include "coredal/ResourceSet.hpp"
#include "coredal/Service.hpp"
#include "coredal/Session.hpp"

#include "appdal/DataReader.hpp"
#include "appdal/DataReaderConf.hpp"
#include "appdal/DataRecorder.hpp"
#include "appdal/DataRecorderConf.hpp"

#include "appdal/ReadoutModule.hpp"
#include "appdal/DLH.hpp"
#include "appdal/TPHandlerModule.hpp"
#include "appdal/FragmentAggregator.hpp"
#include "appdal/ReadoutModuleConf.hpp"
#include "appdal/NetworkConnectionRule.hpp"
#include "appdal/NetworkConnectionDescriptor.hpp"
#include "appdal/QueueConnectionRule.hpp"
#include "appdal/QueueDescriptor.hpp"
#include "appdal/ReadoutApplication.hpp"
#include "appdal/TPRequestHandler.hpp"

#include "appdal/appdalIssues.hpp"

#include "logging/Logging.hpp"

#include <string>
#include <vector>

using namespace dunedaq;
using namespace dunedaq::appdal;

static ModuleFactory::Registrator
__reg__("ReadoutApplication", [] (const SmartDaqApplication* smartApp,
                                  oksdbinterfaces::Configuration* confdb,
                                  const std::string& dbfile,
                                  const coredal::Session* session) -> ModuleFactory::ReturnType
  {
    auto app = smartApp->cast<ReadoutApplication>();
    return app->generate_modules(confdb, dbfile, session);
  });

std::vector<const coredal::DaqModule*> 
ReadoutApplication::generate_modules(oksdbinterfaces::Configuration* confdb,
                                     const std::string& dbfile,
                                     const coredal::Session* session) const {
  //oks::OksFile::set_nolock_mode(true);

  std::vector<const coredal::DaqModule*> modules;

  auto dlhConf = get_link_handler();
  auto dlhClass = dlhConf->get_template_for();

  // Process the queue rules looking for inputs to our DL/TP handler modules
  const QueueDescriptor* dlhInputQDesc = nullptr;
  const QueueDescriptor* dlhReqInputQDesc = nullptr;
  const QueueDescriptor* tpInputQDesc = nullptr;
  const QueueDescriptor* tpReqInputQDesc = nullptr;
  const QueueDescriptor* faOutputQDesc = nullptr;

  for (auto rule : get_queue_rules()) {
    auto destination_class = rule->get_destination_class();
    auto data_type = rule->get_descriptor()->get_data_type();
    if (destination_class == "DLH" || destination_class == dlhClass) {
      if (data_type == "DataRequest") {
	      dlhReqInputQDesc = rule->get_descriptor();
      }
      else {
	      dlhInputQDesc = rule->get_descriptor();
      }
    }
    else if (destination_class == "TPHandlerModule") {
      if (data_type == "DataRequest") {
	      tpReqInputQDesc = rule->get_descriptor();
      }
      else {
	      tpInputQDesc = rule->get_descriptor();
      }
    }
    else if (destination_class == "FragmentAggregator") {
      faOutputQDesc = rule->get_descriptor();
    }
  }
  // Process the network rules looking for the Fragment Aggregator and TP handler data reuest inputs
  const NetworkConnectionDescriptor* faNetDesc = nullptr;
  const NetworkConnectionDescriptor* tpNetDesc = nullptr;
  const NetworkConnectionDescriptor* tsNetDesc = nullptr;
  for (auto rule : get_network_rules()) {
    auto endpoint_class = rule->get_endpoint_class();
    if (endpoint_class == "FragmentAggregator" ) {
      faNetDesc = rule->get_descriptor();
    }
    else if (endpoint_class == "TPHandlerModule") {
      tpNetDesc = rule->get_descriptor(); // this is the connection publishing TPSets!
    }
    else if (endpoint_class == "DLH" || endpoint_class == dlhClass) {
	tsNetDesc = rule->get_descriptor();
    }
  }

  // Create here the Queue on which all data fragments are forwarded to the fragment aggregator
  // and a container for the queues of data request to TP handler and DLH
  if (faOutputQDesc == nullptr) {
    throw (BadConf(ERS_HERE, "No fragment output queue descriptor given"));
  }
  oksdbinterfaces::ConfigObject faQueueObj;
  std::vector<const coredal::Connection*> faOutputQueues;


  std::string taFragQueueUid("reqToFA-"+UID());
  confdb->create(dbfile, "Queue", taFragQueueUid, faQueueObj);
  faQueueObj.set_by_val<std::string>("data_type", faOutputQDesc->get_data_type());
  faQueueObj.set_by_val<std::string>("queue_type", faOutputQDesc->get_queue_type());
  faQueueObj.set_by_val<uint32_t>("capacity", faOutputQDesc->get_capacity());

  // Now create the TP Handler and its associated queue and network
  // connections if we have a TP handler config
  oksdbinterfaces::ConfigObject tpReqQueueObj;
  oksdbinterfaces::ConfigObject tpQueueObj;
  oksdbinterfaces::ConfigObject tpNetObj;
  auto tpHandlerConf = get_tp_handler();
  if (tpHandlerConf) {
    if (tpNetDesc == nullptr) {
      throw (BadConf(ERS_HERE, "No tpHandler network descriptor given"));
    }
    if (tpInputQDesc == nullptr) {
      throw (BadConf(ERS_HERE, "No tpHandler data input queue descriptor given"));
    }
    if (tpReqInputQDesc == nullptr) {
      throw (BadConf(ERS_HERE, "No tpHandler data request queue descriptor given"));
    }
    if (tsNetDesc == nullptr) {
      throw (BadConf(ERS_HERE, "No timesync output network descriptor given"));
    }
    
    auto tpsrc = get_tp_src_id();
    if (tpsrc == 0) {
      throw (BadConf(ERS_HERE, "No TPHandler src_id given"));
    }
    std::string tpQueueUid("inputToTPH-"+std::to_string(tpsrc));
    confdb->create(dbfile, "Queue", tpQueueUid, tpQueueObj);
    tpQueueObj.set_by_val<std::string>("data_type", tpInputQDesc->get_data_type());
    tpQueueObj.set_by_val<std::string>("queue_type", tpInputQDesc->get_queue_type());
    tpQueueObj.set_by_val<uint32_t>("capacity", tpInputQDesc->get_capacity());

    std::string tpReqQueueUid("ReqToTPH-"+std::to_string(tpsrc));
    confdb->create(dbfile, "Queue", tpReqQueueUid, tpReqQueueObj);
    tpReqQueueObj.set_by_val<std::string>("data_type", tpReqInputQDesc->get_data_type());
    tpReqQueueObj.set_by_val<std::string>("queue_type", tpReqInputQDesc->get_queue_type());
    tpReqQueueObj.set_by_val<uint32_t>("capacity", tpReqInputQDesc->get_capacity());
    faOutputQueues.push_back(confdb->get<coredal::Connection>(tpReqQueueUid));

    auto tpServiceObj = tpNetDesc->get_associated_service()->config_object();
    std::string tpStreamUid("tpstream-"+UID());
    confdb->create(dbfile, "NetworkConnection", tpStreamUid, tpNetObj);
    tpNetObj.set_by_val<std::string>("connection_type", tpNetDesc->get_connection_type());
    tpNetObj.set_obj("associated_service", &tpServiceObj);
    
    auto tphConfObj = tpHandlerConf->config_object();
    oksdbinterfaces::ConfigObject tpObj;
    std::string tpUid("tphandler-"+std::to_string(tpsrc));
    confdb->create(dbfile, "TPHandlerModule", tpUid, tpObj);
    tpObj.set_by_val<uint32_t>("source_id", tpsrc);
    tpObj.set_obj("module_configuration", &tphConfObj);
    tpObj.set_objs("inputs", {&tpQueueObj, &tpReqQueueObj});
    tpObj.set_objs("outputs", {&tpNetObj, &faQueueObj});

    // Add to our list of modules to return
    modules.push_back(confdb->get<TPHandlerModule>(tpUid));
  }

  // Now create the DataReader objects, one per group of data streams
  auto rdrConf = get_data_reader();
  if (rdrConf == 0) {
    throw (BadConf(ERS_HERE, "No DataReader configuration given"));
  }
  if (dlhInputQDesc == nullptr) {
    throw (BadConf(ERS_HERE, "No DLH data input queue descriptor given"));
  }
  if (dlhReqInputQDesc == nullptr) {
    throw (BadConf(ERS_HERE, "No DLH request input queue descriptor given"));
  }

  int rnum = 0;
  // Create a DataReader for each (non-disabled) group and a Data Link
  // Handler for each stream of this DataReader
  //for (auto roGroup : get_readout_groups()) {
  for (auto roGroup : get_contains()) {
    if (roGroup->disabled(*session)) {
      TLOG_DEBUG(7) << "Ignoring disabled ReadoutGroup " << roGroup->UID();
      continue;
    }
    // get the readout groups and the interfaces and streams therein; 1 reaout group corresponds to 1 data reader module
    auto rset = roGroup->cast<coredal::ReadoutGroup>();
    if (rset == nullptr) {
        throw (BadConf(ERS_HERE, "ReadoutApplication contains something other than ReadoutGroup"));
    }
    std::vector<const coredal::Connection*> outputQueues;
    if (rset->get_contains().empty()) {
        throw (BadConf(ERS_HERE, "ReadoutGroup does not contain interfaces"));
    }
    auto interfaces = rset->get_contains();
    for (auto res_set : interfaces) {
      if (res_set->disabled(*session)) {
        TLOG_DEBUG(7) << "Ignoring disabled ReadoutInterface " << res_set->UID();
        continue;
      }
      auto interface = res_set->cast<coredal::ReadoutInterface>();
      if (interface == nullptr) {
        throw (BadConf(ERS_HERE, "ReadoutGroup contains something othen than ReadoutInterface"));
      }
      for (auto res : interface->get_contains()) {
         auto stream = res->cast<coredal::DROStreamConf>();
         if (stream == nullptr) {
           throw (BadConf(ERS_HERE, "ReadoutInterface contains something other than DROStreamConf"));
         }
         if (stream->disabled(*session)) {
           TLOG_DEBUG(7) << "Ignoring disabled DROStreamConf " << stream->UID();
           continue;
         }
         auto id = stream->get_src_id();
         auto det_id = stream->get_geo_id()->get_detector_id();
         std::string uid("DLH-"+std::to_string(id));
         oksdbinterfaces::ConfigObject dlhObj;
         TLOG_DEBUG(7) <<  "creating OKS configuration object for Data Link Handler class " << dlhClass;
         confdb->create(dbfile, dlhClass, uid, dlhObj);
         dlhObj.set_by_val<uint32_t>("source_id", id);
         dlhObj.set_by_val<uint32_t>("detector_id", det_id);
         dlhObj.set_obj("module_configuration", &dlhConf->config_object());

         // Time Sync network connection
         if (dlhConf->get_generate_timesync()) {
          std::string tsStreamUid("timesync"+std::to_string(id));
          auto tsServiceObj = tsNetDesc->get_associated_service()->config_object();
          oksdbinterfaces::ConfigObject tsNetObj;
          confdb->create(dbfile, "NetworkConnection", tsStreamUid, tsNetObj);
          tsNetObj.set_by_val<std::string>("connection_type", tsNetDesc->get_connection_type());
          tsNetObj.set_obj("associated_service", &tsServiceObj);

          if (tpHandlerConf) {
            dlhObj.set_objs("outputs", {&tpQueueObj, &faQueueObj, &tsNetObj});
          }
          else {
            dlhObj.set_objs("outputs", {&faQueueObj, &tsNetObj});
          }
         }
         else {
          if (tpHandlerConf) {
           dlhObj.set_objs("outputs", {&tpQueueObj, &faQueueObj});
          }
          else {
	          dlhObj.set_objs("outputs", {&faQueueObj});
          }
         }
         std::string dataQueueUid("inputToDLH-"+std::to_string(id));
         oksdbinterfaces::ConfigObject queueObj;
         confdb->create(dbfile, "QueueWithId", dataQueueUid, queueObj);
         queueObj.set_by_val<std::string>("data_type", dlhInputQDesc->get_data_type());
         queueObj.set_by_val<std::string>("queue_type", dlhInputQDesc->get_queue_type());
         queueObj.set_by_val<uint32_t>("capacity", dlhInputQDesc->get_capacity());
         queueObj.set_by_val<uint32_t>("id", stream->get_src_id());

         std::string reqQueueUid("inputReqToDLH-"+std::to_string(id));
         oksdbinterfaces::ConfigObject reqQueueObj;
         confdb->create(dbfile, "Queue", reqQueueUid, reqQueueObj);
         reqQueueObj.set_by_val<std::string>("data_type", dlhReqInputQDesc->get_data_type());
         reqQueueObj.set_by_val<std::string>("queue_type", dlhReqInputQDesc->get_queue_type());
         reqQueueObj.set_by_val<uint32_t>("capacity", dlhReqInputQDesc->get_capacity());
         // Add the requessts queue dal pointer to the outputs of the FragmentAggregator
         faOutputQueues.push_back(confdb->get<coredal::Connection>(reqQueueUid));

         dlhObj.set_objs("inputs", {&queueObj, &reqQueueObj});

         // Add the input queue dal pointer to the outputs of the DataReader
         outputQueues.push_back(confdb->get<coredal::Connection>(dataQueueUid));

         modules.push_back(confdb->get<DLH>(uid));
       }
    }

    std::string readerUid("datareader-"+UID()+"-"+std::to_string(rnum++));
    std::string readerClass = rdrConf->get_template_for();
    oksdbinterfaces::ConfigObject readerObj;
    TLOG_DEBUG(7) <<  "creating OKS configuration object for Data reader class " << readerClass;
    confdb->create(dbfile, readerClass, readerUid, readerObj);

    std::vector<const oksdbinterfaces::ConfigObject*> qObjs;
    for (auto q : outputQueues) {
      qObjs.push_back(&q->config_object());
    }
    readerObj.set_objs("outputs", qObjs);
    readerObj.set_obj("configuration", &rdrConf->config_object());
    readerObj.set_obj("readout_group", &rset->config_object());

    modules.push_back(confdb->get<DataReader>(readerUid));
  }

  // Finally create Fragment Aggregator
  std::string faUid("fragmentaggregator-"+UID());
  oksdbinterfaces::ConfigObject faObj;
  TLOG_DEBUG(7) <<  "creating OKS configuration object for Fragment Aggregator class ";
  confdb->create(dbfile, "FragmentAggregator", faUid, faObj);

  //Add network connection to TRBs
  auto faServiceObj = faNetDesc->get_associated_service()->config_object();
  std::string faNetUid("fragmentrequests-"+UID());;
  oksdbinterfaces::ConfigObject faNetObj;
  confdb->create(dbfile, "NetworkConnection", faNetUid, faNetObj);
  tpNetObj.set_by_val<std::string>("connection_type", faNetDesc->get_connection_type());
  tpNetObj.set_obj("associated_service", &faServiceObj);

  //Add output queueus of data requests
  std::vector<const oksdbinterfaces::ConfigObject*> qObjs;
  for (auto q : faOutputQueues) {
    qObjs.push_back(&q->config_object());
  }
  faObj.set_objs("inputs", {&faNetObj, &faQueueObj});
  faObj.set_objs("outputs", qObjs);

  modules.push_back(confdb->get<FragmentAggregator>(faUid));

  //oks::OksFile::set_nolock_mode(false);
  return modules;
}
