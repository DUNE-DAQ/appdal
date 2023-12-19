/**
 * @file DFApplication.cpp
 *
 * Implementation of DFApplication's generate_modules dal method
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2023.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "ModuleFactory.hpp"

#include "oksdbinterfaces/Configuration.hpp"
#include "oks/kernel.hpp"
#include "coredal/Connection.hpp"
#include "coredal/NetworkConnection.hpp"
#include "appdal/TRBuilder.hpp"
#include "appdal/TRBConf.hpp"
#include "appdal/DataWriter.hpp"
#include "appdal/DataWriterConf.hpp"
#include "appdal/DFApplication.hpp"
#include "appdal/ReadoutApplication.hpp"
#include "appdal/NetworkConnectionRule.hpp"
#include "appdal/NetworkConnectionDescriptor.hpp"
#include "appdal/QueueConnectionRule.hpp"
#include "appdal/QueueDescriptor.hpp"
#include "appdal/appdalIssues.hpp"
#include "logging/Logging.hpp"

#include <string>
#include <vector>

using namespace dunedaq;
using namespace dunedaq::appdal;

static ModuleFactory::Registrator
__reg__("DFApplication", [] (const SmartDaqApplication* smartApp,
                             oksdbinterfaces::Configuration* confdb,
                             const std::string& dbfile,
                             const coredal::Session* session) -> ModuleFactory::ReturnType
  {
    auto app = smartApp->cast<DFApplication>();
    return app->generate_modules(confdb, dbfile, session);
  }
  );

inline void
fill_queue_object_from_desc(const QueueDescriptor* qDesc, oksdbinterfaces::ConfigObject& qObj)
{
  qObj.set_by_val<std::string>("data_type", qDesc->get_data_type());
  qObj.set_by_val<std::string>("queue_type", qDesc->get_queue_type());
  qObj.set_by_val<uint32_t>("capacity", qDesc->get_capacity());
}

inline void
fill_netconn_object_from_desc(const NetworkConnectionDescriptor* netDesc, oksdbinterfaces::ConfigObject& netObj)
{
  netObj.set_by_val<std::string>("data_type", netDesc->get_data_type());
  netObj.set_by_val<std::string>("connection_type", netDesc->get_connection_type());
  netObj.set_by_val<std::string>("uri", netDesc->get_uri());
  netObj.set_by_val<uint16_t>("port", netDesc->get_port());
}

std::vector<const coredal::DaqModule*> 
DFApplication::generate_modules(oksdbinterfaces::Configuration* confdb,
                                     const std::string& dbfile,
                                     const coredal::Session* session) const
{
  std::vector<const coredal::DaqModule*> modules;

  // Containers for module specific config objects for output/input
  // Prepare TRB output objects 
  std::vector<const oksdbinterfaces::ConfigObject*> trbOutputObjs;

  // -- First, we process expected Queue and Network connections and create their objects.

  // Process the queue rules looking for the TriggerRecord queue between TRB and DataWriter
  const QueueDescriptor* trQDesc = nullptr;
  for (auto rule : get_queue_rules()) {
    auto destination_class = rule->get_destination_class();
    if (destination_class == "DataWriter") {
      trQDesc = rule->get_descriptor();
    }
  }
  if (trQDesc == nullptr) { // BadConf if no descriptor between TRB and DataWriter
    throw (BadConf(ERS_HERE, "Could not find queue descriptor rule for TriggerRecords!"));
  }
  // Create queue connection config object
  oksdbinterfaces::ConfigObject trQueueObj;
  std::string trQueueUid("trigger-records-"+UID());
  confdb->create(dbfile, "Queue", trQueueUid, trQueueObj);
  fill_queue_object_from_desc(trQDesc, trQueueObj);
  // Place trigger record queue object into vector of output objs of TRB module
  trbOutputObjs.push_back(&trQueueObj); 

  // Process the network rules looking for the Fragments and TriggerDecision inputs for TRB
  const NetworkConnectionDescriptor* fragNetDesc = nullptr;
  const NetworkConnectionDescriptor* trigdecNetDesc = nullptr; 
  for (auto rule : get_network_rules()) {
    auto endpoint_class = rule->get_endpoint_class();
    auto descriptor = rule->get_descriptor();
    auto data_type = descriptor->get_data_type();
    if (endpoint_class == "TRBuilder" && data_type == "Fragment") {
      fragNetDesc = rule->get_descriptor();
    }
    else if (endpoint_class == "TRBuilder" && data_type == "TriggerDecision") {
      trigdecNetDesc = rule->get_descriptor();
    }
  }
  if (fragNetDesc == nullptr) { // BadConf if no descriptor for Fragments into TRB
    throw (BadConf(ERS_HERE, "Could not find network descriptor rule for input Fragments!"));
  }
  if (trigdecNetDesc == nullptr) { // BadCond if no descriptor for TriggerDecisions into TRB
    throw (BadConf(ERS_HERE, "Could not find network descriptor rule for input TriggerDecisions!"));
  }
  // Create network connection config object
  oksdbinterfaces::ConfigObject fragNetObj;
  oksdbinterfaces::ConfigObject trigdecNetObj;
  std::string fragNetUid("fragments-to-dataflow-"+UID());
  std::string trigdecNetUid("tigger-decisions-"+UID());
  confdb->create(dbfile, "NetworkConnection", fragNetUid, fragNetObj);
  confdb->create(dbfile, "NetworkConnection", trigdecNetUid, trigdecNetObj);
  fill_netconn_object_from_desc(fragNetDesc, fragNetObj);
  fill_netconn_object_from_desc(trigdecNetDesc, trigdecNetObj);

  // Process special Network rules! 
  // Looking for DataRequest rules from ReadoutAppplications in current Session
  auto sessionApps = session->get_all_applications();
  for (auto app : sessionApps) {
    auto smartapp = app->cast<appdal::SmartDaqApplication>();
    if (smartapp != nullptr) {
      auto roapp = smartapp->cast<appdal::ReadoutApplication>();
      if (roapp != nullptr) {
        TLOG_DEBUG(7) << "Readout app in session: " << roapp;
	auto roQRules = smartapp->get_network_rules();
        for (auto rule : roQRules) {
          auto descriptor = rule->get_descriptor(); 
          auto data_type = descriptor->get_data_type();
	  if (data_type == "DataRequest") {
            oksdbinterfaces::ConfigObject dreqNetObj;
	    std::string dreqNetUid("data-request-to-"+roapp->UID()+"-"+UID());
	    confdb->create(dbfile, "NetworkConnection", dreqNetUid, dreqNetObj);
	    fill_netconn_object_from_desc(descriptor, dreqNetObj);
            trbOutputObjs.push_back(&dreqNetObj);
	  } // If network rule has DataRequest type of data
	} // Loop over ReadoutApps network rules 
      } // if smartapp is ReadoutApplication
    } // if smartapp cast success
  } // loop over Session specific Apps

  // -- Second, we create the Module objects and assign their configs, with the precreated 
  // -- connection config objects above.

  // Get TRB Config Object
  auto trbConf = get_trb();
  if (trbConf == 0) {
    throw (BadConf(ERS_HERE, "No DataWriter or TRB configuration given"));
  }
  auto trbConfObj = trbConf->config_object();
  // Prepare TRB Module Object and assign its Config Object.
  oksdbinterfaces::ConfigObject trbObj;
  std::string trbUid("trbuilder-"+UID());
  confdb->create(dbfile, "TRBuilder", trbUid, trbObj);
  trbObj.set_obj("configuration", &trbConfObj);
  trbObj.set_objs("inputs", {&trigdecNetObj, &fragNetObj});
  trbObj.set_objs("outputs", trbOutputObjs);
  // Push TRB Module Object from confdb
  modules.push_back(confdb->get<TRBuilder>(trbUid));


  // Get DataWriter Config Object
  auto dwrConf = get_data_writer();
  if (dwrConf == 0) {
    throw (BadConf(ERS_HERE, "No DataWriter or TRB configuration given"));
  }
  auto dwrConfObj = dwrConf->config_object();
  // Prepare DataWriter Module Object and assign its Config Object.
  oksdbinterfaces::ConfigObject dwrObj;
  std::string dwrUid("datawriter-"+UID());
  confdb->create(dbfile, "DataWriter", dwrUid, dwrObj);
  dwrObj.set_obj("configuration", &dwrConfObj);
  dwrObj.set_objs("inputs", {&trQueueObj});
  // Push DataWriter Module Object from confdb
  modules.push_back(confdb->get<DataWriter>(dwrUid));

  return modules;
}
