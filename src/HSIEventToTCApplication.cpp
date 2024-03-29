/**
 * @file DFO.cpp
 *
 * Implementation of DFOApplication's generate_modules dal method
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
#include "appdal/DataSubscriber.hpp"
#include "appdal/HSIEventToTCApplication.hpp"
#include "appdal/HSI2TCTranslatorConf.hpp"
#include "appdal/NetworkConnectionRule.hpp"
#include "appdal/NetworkConnectionDescriptor.hpp"
#include "appdal/QueueConnectionRule.hpp"
#include "appdal/QueueDescriptor.hpp"
#include "coredal/Service.hpp"
#include "appdal/appdalIssues.hpp"
#include "logging/Logging.hpp"

#include <string>
#include <vector>

using namespace dunedaq;
using namespace dunedaq::appdal;

static ModuleFactory::Registrator
__reg__("HSIEventToTCApplication", [] (const SmartDaqApplication* smartApp,
                             oksdbinterfaces::Configuration* confdb,
                             const std::string& dbfile,
                             const coredal::Session* session) -> ModuleFactory::ReturnType
  {
    auto app = smartApp->cast<HSIEventToTCApplication>();
    return app->generate_modules(confdb, dbfile, session);
  }
  );

std::vector<const coredal::DaqModule*> 
HSIEventToTCApplication::generate_modules(oksdbinterfaces::Configuration* confdb,
                                     const std::string& dbfile,
                                     const coredal::Session* session) const
{
  std::vector<const coredal::DaqModule*> modules;


  std::string hstcUid("module-" + UID());
  oksdbinterfaces::ConfigObject hstcObj;
  TLOG_DEBUG(7) << "creating OKS configuration object for the DataSubscriber class ";
  confdb->create(dbfile, "DataSubscriber", hstcUid, hstcObj);

  auto hstcConf = get_hsevent_to_tc_conf();
  hstcObj.set_obj("configuration", &hstcConf->config_object());

  if (hstcConf == 0) {
    throw(BadConf(ERS_HERE, "No HSI2TCTranslatorConf configuration given"));
  }

  oksdbinterfaces::ConfigObject inObj;
  oksdbinterfaces::ConfigObject outObj;

  for (auto rule : get_network_rules()) {
    auto endpoint_class = rule->get_endpoint_class();
    auto descriptor = rule->get_descriptor();

    oksdbinterfaces::ConfigObject connObj;
    auto serviceObj = descriptor->get_associated_service()->config_object();
    std::string connUid(descriptor->get_uid_base());
    if (descriptor->get_data_type() == "HSIEvent") {
        confdb->create(dbfile, "NetworkConnection", connUid, connObj);
        connObj.set_by_val<std::string>("data_type", descriptor->get_data_type());
        connObj.set_by_val<std::string>("connection_type", descriptor->get_connection_type());
        connObj.set_obj("associated_service", &serviceObj);

        inObj = connObj;
    } 
    else if (descriptor->get_data_type() == "TriggerCandidate") {
        confdb->create(dbfile, "NetworkConnection", connUid+UID(), connObj);
        connObj.set_by_val<std::string>("data_type", descriptor->get_data_type());
        connObj.set_by_val<std::string>("connection_type", descriptor->get_connection_type());
        connObj.set_obj("associated_service", &serviceObj);

    	outObj = connObj;
    }
  } 

  if (inObj == nullptr) {
    throw(BadConf(ERS_HERE, "No HSIEvent input connection descriptor given"));
  }
  if (outObj == nullptr) {
    throw(BadConf(ERS_HERE, "No TriggerCandidate output connection descriptor given"));
  }

  hstcObj.set_objs("inputs", {&inObj});
  hstcObj.set_objs("outputs", {&outObj});

  // Add to our list of modules to return
  modules.push_back(confdb->get<DataSubscriber>(hstcUid));

  return modules;
}
