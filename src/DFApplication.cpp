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
#include "appdal/DFApplication.hpp"
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

std::vector<const coredal::DaqModule*> 
DFApplication::generate_modules(oksdbinterfaces::Configuration* confdb,
                                     const std::string& dbfile,
                                     const coredal::Session* session) const
{
  std::vector<const coredal::DaqModule*> modules;

  // Code to actually generate modules should go here
  std::cout << __FUNCTION__ << " -- Unimplemented function\n";

  return modules;
}
