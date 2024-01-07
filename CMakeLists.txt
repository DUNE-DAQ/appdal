cmake_minimum_required(VERSION 3.12)
project(appdal VERSION 0.1.0)

find_package(daq-cmake REQUIRED)

daq_setup_environment()

find_package(okssystem REQUIRED)
find_package(oks REQUIRED)
find_package(genconfig REQUIRED)
find_package(ers REQUIRED)
find_package(logging REQUIRED)
find_package(oksdbinterfaces REQUIRED)
find_package(coredal REQUIRED)


#find_package(Boost COMPONENTS unit_test_framework REQUIRED)

daq_oks_codegen(application.schema.xml fdmodules.schema.xml
  NAMESPACE dunedaq::appdal DEP_PKGS coredal)

daq_add_library(ReadoutApplication.cpp SmartDaqApplication.cpp
  DFApplication.cpp DFOApplication.cpp TPWriterApplication.cpp
 LINK_LIBRARIES oksdbinterfaces::oksdbinterfaces okssystem::okssystem
  logging::logging coredal::coredal oks::oks ers::ers)


daq_add_python_bindings(dal_methods.cpp module.cpp LINK_LIBRARIES 
  appdal coredal::coredal)


daq_add_application(generate_modules_test generate_modules_test.cxx
 TEST LINK_LIBRARIES appdal coredal::coredal oksdbinterfaces::oksdbinterfaces
 logging::logging)

##############################################################################


# See https://dune-daq-sw.readthedocs.io/en/latest/packages/daq-cmake/#daq_add_unit_test

#daq_add_unit_test(Placeholder_test LINK_LIBRARIES)  # Placeholder_test should be replaced with real unit tests

##############################################################################

daq_install()

