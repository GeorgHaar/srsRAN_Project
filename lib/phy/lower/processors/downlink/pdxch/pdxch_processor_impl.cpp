/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "pdxch_processor_impl.h"
#include "srsran/instrumentation/traces/du_traces.h"
#include "srsran/phy/support/resource_grid_reader.h"
#include "srsran/srsvec/zero.h"

using namespace srsran;

void pdxch_processor_impl::connect(pdxch_processor_notifier& notifier_)
{
  notifier = &notifier_;
}

pdxch_processor_request_handler& pdxch_processor_impl::get_request_handler()
{
  return *this;
}

pdxch_processor_baseband& pdxch_processor_impl::get_baseband()
{
  return *this;
}

bool pdxch_processor_impl::process_symbol(baseband_gateway_buffer_writer&                 samples,
                                          const pdxch_processor_baseband::symbol_context& context)
{
  srsran_assert(notifier != nullptr, "Notifier has not been connected.");

  // Update the current resource grid if the slot has changed.
  if (context.slot != current_slot) {
    // Update slot.
    current_slot = context.slot;

    // Release current grid.
    current_grid.release();

    // Exchange an empty request with the current slot with a stored request.
    auto request = requests.exchange({context.slot, shared_resource_grid()});

    // If the request resource grid pointer is invalid, the request is empty.
    if (!request.grid) {
      return false;
    }

    // If the slot of the request does not match the current slot, then notify a late event.
    if (current_slot != request.slot) {
      resource_grid_context late_context;
      late_context.slot   = request.slot;
      late_context.sector = context.sector;
      notifier->on_pdxch_request_late(late_context);
      return false;
    }

    // Discard the resource grid if there is nothing to transmit.
    if (request.grid.get_reader().is_empty()) {
      return false;
    }

    // Update the current grid with the new resource grid.
    current_grid = std::move(request.grid);
  }

  // Skip processing if the resource grid is invalid.
  if (!current_grid) {
    return false;
  }

  // ================================================================================================
  // SIGNAL MODULATION: Convert resource grid symbols to baseband samples
  // ================================================================================================
  //
  // This is a critical step in the signal flow where frequency-domain resource elements
  // (containing 5G signals like PDCCH, PDSCH, SSB) are converted to time-domain baseband
  // samples that can be transmitted by radio hardware.
  //
  // Process:
  // 1. Get the resource grid containing frequency-domain signals from Upper PHY
  // 2. For each antenna port, call OFDM modulator to convert to time-domain
  // 3. Store the resulting baseband samples in the output buffer
  //
  // The 'samples' buffer will contain the final baseband samples ready for transmission
  //

  // Calculate symbol index within the subframe (0-13 for normal CP, 0-11 for extended CP)
  unsigned symbol_index_subframe = context.symbol + context.slot.subframe_slot_index() * nof_symbols_per_slot;

  // ================================================================================================
  // MULTI-ANTENNA PROCESSING: Modulate each antenna port independently
  // ================================================================================================
  // For MIMO systems, each antenna port has its own resource grid and requires 
  // separate OFDM modulation to generate independent baseband streams
  for (unsigned i_port = 0; i_port != nof_tx_ports; ++i_port) {
    // **KEY FUNCTION CALL**: This performs the OFDM modulation
    // - Input:  current_grid contains frequency-domain symbols (QAM/PSK modulated)
    // - Output: samples.get_channel_buffer(i_port) receives time-domain baseband samples
    // - Process: IFFT + cyclic prefix addition + phase compensation + scaling
    modulator->modulate(samples.get_channel_buffer(i_port), current_grid.get_reader(), i_port, symbol_index_subframe);
  }

  // Return true to indicate successful processing
  // The 'samples' buffer now contains baseband samples ready for transmission to radio hardware
  return true;
}

void pdxch_processor_impl::handle_request(const shared_resource_grid& grid, const resource_grid_context& context)
{
  // Ignore request if the processor has stopped.
  if (stopped) {
    return;
  }

  srsran_assert(notifier != nullptr, "Notifier has not been connected.");

  // Swap the new request by the current request in the circular array.
  auto request = requests.exchange({context.slot, grid.copy()});

  // If there was a request with a resource grid, then notify a late event with the context of the discarded request.
  if (request.grid) {
    resource_grid_context late_context;
    late_context.slot   = request.slot;
    late_context.sector = context.sector;
    notifier->on_pdxch_request_late(late_context);
    l1_dl_tracer << instant_trace_event{"on_pdxch_request_late", instant_trace_event::cpu_scope::thread};
  }
}
