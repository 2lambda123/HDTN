/**
 * @file TelemetryRunner.h
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
 * This TelemetryRunner class launches a thread that polls HDTN telemetry data. It also
 * processes the data and provides interfaces for viewing it.
 */

#ifndef TELEMETRY_RUNNER_H
#define TELEMETRY_RUNNER_H 1

#include "zmq.hpp"

#include "telem_lib_export.h"
#include "TelemetryRunnerProgramOptions.h"


class TelemetryRunner
{
    public:
        TELEM_LIB_EXPORT TelemetryRunner();
        TELEM_LIB_EXPORT ~TelemetryRunner();

        /**
         * Starts the runner as a standalone process 
         */
        TELEM_LIB_EXPORT bool Run(int argc, const char* const argv[], volatile bool & running);

        /**
         * Starts the runner as an async thread
         * @param inprocContextPtr context to use for the inproc zmq connections
         * @param options program options for the runner
         */
        TELEM_LIB_EXPORT bool Init(zmq::context_t *inprocContextPtr, TelemetryRunnerProgramOptions& options);

        /**
         * Determines whether the runner wants to exits 
         */
        TELEM_LIB_EXPORT bool ShouldExit();

        /**
         * Stops the runner, if running as an async tread 
         */
        TELEM_LIB_EXPORT void Stop();

    private:
        // Internal implementation class and pointer
        struct Impl;
        std::unique_ptr<Impl> m_pimpl;
};

#endif //TELEMETRY_RUNNER_H
