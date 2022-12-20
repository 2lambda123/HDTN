/**
 * @file TelemetryRunnerProgramOptions.h
 *
 * @copyright Copyright Ⓒ 2021 United States Government as represented by
 * the National Aeronautics and Space Administration.
 * No copyright is claimed in the United States under Title 17, U.S.Code.
 * All Other Rights Reserved.
 *
 * @section LICENSE
 * Released under the NASA Open Source Agreement (NOSA)
 * See LICENSE.md in the source root directory for more information.
 *
 * @section DESCRIPTION
 *
 * This TelemetryRunnerProgramOptions class stores the program options for the TelemetryRunner
 */

#ifndef TELEMETRY_RUNNER_PROGRAM_OPTIONS_H
#define TELEMETRY_RUNNER_PROGRAM_OPTIONS_H 1

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "telem_lib_export.h"

class TELEM_LIB_EXPORT TelemetryRunnerProgramOptions
{
    public:
        TelemetryRunnerProgramOptions();

        /**
         * Appends program options, specific to this TelemetryRunner, to an options descroption 
         */
        static void AppendToDesc(boost::program_options::options_description& desc);

        /**
         * Parses a variable map and stores the result in this TelemetryRunnerProgramOptions 
         */
        bool ParseFromVariableMap(boost::program_options::variables_map vm);

        boost::filesystem::path m_guiDocumentRoot;
        std::string m_guiPortNumber;

    private:
        boost::filesystem::path GetDocumentRootAndValidate(boost::program_options::variables_map vm);
        std::string GetPortNumberAsString(boost::program_options::variables_map vm);
};

#endif // TELEMETRY_RUNNER_PROGRAM_OPTIONS_H
