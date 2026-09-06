// *****************************************************************************
//
// Parsing logic derived from septentrio_gnss_driver 1.4.7
// parsers/sbf_blocks.hpp (BSD-3-Clause), rewritten from boost::spirit::qi to
// bounds-checked little-endian reads; ROS axis flips removed (native
// Septentrio conventions). Full license: 3rd_party/septentrio_gnss_driver/LICENSE.
//
// *****************************************************************************

#pragma once

#include <cstddef>
#include <cstdint>

#include "sbf_types.h"


namespace asterx::sbf {
    // Input is one complete SBF frame as delivered by SsnRx ($@ header + CRC;
    // the CRC has already been validated by ssnrx, it is not re-checked here).
    // On failure (truncated frame / wrong ID / malformed sub-blocks) the parsers
    // return false and the output struct contents are unspecified.

    // Block id of a frame, without parsing (0 when the buffer is too short)
    std::uint16_t PeekId(const std::uint8_t *data, std::size_t size);

    bool ParseBlockHeader(const std::uint8_t *data, std::size_t size, BlockHeader &out);

    bool ParseINSNavGeod(const std::uint8_t *data, std::size_t size, INSNavGeod &out);

    bool ParseReceiverStatus(const std::uint8_t *data, std::size_t size, ReceiverStatus &out);
} // namespace asterx::sbf
