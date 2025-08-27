/*
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

/// \file
/// \brief Signal Flow Example: How srsRAN generates and transmits signals
///
/// This example demonstrates the complete signal flow in srsRAN from main() to baseband transmission:
///
/// 1. **Application Startup**: Initialize components and establish connections
/// 2. **Upper PHY Layer**: Generate 5G signals (SSB, PDCCH, PDSCH) in resource grids  
/// 3. **Lower PHY Layer**: Perform OFDM modulation to convert to baseband samples
/// 4. **Baseband Gateway**: Interface between PHY and radio hardware
/// 5. **Radio Hardware**: Transmit baseband samples over RF
///
/// The purpose of this file is to provide a comprehensive explanation of the signal processing
/// architecture and data flow within srsRAN. Each section is annotated to show exactly how
/// signals are generated, processed, and transmitted.

#include <iostream>
#include <string>

namespace srsran_signal_flow_example {

/// \brief Signal Flow Architecture Overview
///
/// srsRAN implements a layered architecture for 5G signal processing:
///
/// ```
/// ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
/// │  Main App       │    │  Upper PHY      │    │  Lower PHY      │    │ Baseband Gateway│
/// │  (gnb.cpp)      │───▶│  Signal Gen     │───▶│  OFDM Modulator │───▶│  Radio Interface│
/// │                 │    │  Resource Grid  │    │  Baseband Proc  │    │  (UHD/ZMQ)      │
/// └─────────────────┘    └─────────────────┘    └─────────────────┘    └─────────────────┘
///          │                       │                       │                       │
///          ▼                       ▼                       ▼                       ▼
///   Initialize &              5G Signal                OFDM                  Radio Hardware
///   Connect                   Generation           Modulation              Transmission
/// ```
class SignalFlowExplanation {
public:
    /// \brief Step 1: Application Initialization and Component Creation
    ///
    /// The main application (apps/gnb/gnb.cpp or apps/examples/phy/radio_ssb.cpp) performs:
    ///
    /// 1. **Radio Factory Creation**: Creates appropriate radio backend (UHD, ZMQ, etc.)
    /// 2. **Lower PHY Creation**: Initializes OFDM modulators, baseband processors  
    /// 3. **Upper PHY Creation**: Sets up signal generators, channel processors
    /// 4. **Component Connection**: Establishes data flow paths between layers
    ///
    /// \code{.cpp}
    /// // Create radio hardware interface
    /// std::unique_ptr<radio_factory> factory = create_radio_factory("uhd");
    /// std::unique_ptr<radio_session> radio = factory->create(radio_config, executor, notifier);
    ///
    /// // Create lower physical layer (OFDM modulation, baseband processing)  
    /// std::unique_ptr<lower_phy> lower_phy_instance = create_lower_phy(phy_config);
    ///
    /// // Create upper physical layer (5G signal generation)
    /// std::unique_ptr<upper_phy_ssb_example> upper_phy = upper_phy_ssb_example::create(config);
    ///
    /// // Connect signal processing chain
    /// rg_gateway_adapter.connect(&lower_phy_instance->get_rg_handler());
    /// \endcode
    void step1_application_initialization() {
        std::cout << "Step 1: Application initializes radio, lower PHY, and upper PHY components\n";
    }

    /// \brief Step 2: Upper PHY Signal Generation  
    ///
    /// The Upper PHY layer generates 5G signals and places them in resource grids:
    ///
    /// **Signal Types Generated:**
    /// - **SSB (Synchronization Signal Block)**: PSS, SSS, PBCH for cell identification
    /// - **PDCCH (Physical Downlink Control Channel)**: Control information
    /// - **PDSCH (Physical Downlink Shared Channel)**: User data
    /// - **Reference Signals**: For channel estimation
    ///
    /// **Processing Steps:**
    /// 1. **Channel Coding**: Apply LDPC encoding, rate matching
    /// 2. **Scrambling**: Pseudo-random sequence applied to data
    /// 3. **Modulation Mapping**: Convert bits to complex symbols (QPSK, QAM16, QAM64, QAM256)
    /// 4. **Resource Element Mapping**: Place symbols in time-frequency resource grid
    ///
    /// \code{.cpp}
    /// // Example from pdcch_modulator_impl.cpp
    /// void pdcch_modulator_impl::modulate(resource_grid_writer& grid, span<const uint8_t> data, const config_t& config)
    /// {
    ///     // Apply scrambling to input data
    ///     scramble(b_hat, data, config);
    ///     
    ///     // Apply QPSK modulation mapping  
    ///     modulate(d_pdcch.get_slice(0), b_hat, config.scaling);
    ///     
    ///     // Map modulated symbols to resource grid
    ///     map(grid, d_pdcch, config);
    /// }
    /// \endcode
    void step2_upper_phy_signal_generation() {
        std::cout << "Step 2: Upper PHY generates 5G signals and maps them to resource grids\n";
    }

    /// \brief Step 3: Lower PHY OFDM Modulation
    ///
    /// The Lower PHY converts frequency-domain resource grids to time-domain baseband samples:
    ///
    /// **OFDM Modulation Process:**
    /// 1. **Frequency Domain Arrangement**: Extract symbols from resource grid
    /// 2. **IFFT Processing**: Convert frequency domain to time domain  
    /// 3. **Phase Compensation**: Apply symbol-specific phase corrections
    /// 4. **Cyclic Prefix Addition**: Add guard interval for multipath protection
    /// 5. **Scaling and Amplitude Control**: Adjust signal levels for transmission
    ///
    /// \code{.cpp}
    /// // Example from ofdm_modulator_impl.cpp  
    /// void ofdm_symbol_modulator_impl::modulate(span<cf_t> output, const resource_grid_reader& grid, unsigned port_index, unsigned symbol_index)
    /// {
    ///     // Extract frequency-domain data from resource grid
    ///     grid.get(dft->get_input().last(rg_size / 2), port_index, symbol_index % nsymb, 0);
    ///     grid.get(dft->get_input().first(rg_size / 2), port_index, symbol_index % nsymb, rg_size / 2);
    ///     
    ///     // Perform IFFT to convert to time domain
    ///     span<const cf_t> dft_output = dft->run();
    ///     
    ///     // Apply phase compensation and scaling
    ///     cf_t phase_compensation = phase_compensation_table.get_coefficient(symbol_index);
    ///     srsvec::sc_prod(dft_output, phase_compensation * scale, output.last(dft_size));
    ///     
    ///     // Add cyclic prefix
    ///     srsvec::copy(output.first(cp_len), output.last(cp_len));
    /// }
    /// \endcode
    void step3_lower_phy_ofdm_modulation() {
        std::cout << "Step 3: Lower PHY performs OFDM modulation to create time-domain baseband samples\n";
    }

    /// \brief Step 4: Baseband Processing and RF Conditioning
    ///
    /// The baseband processor applies final conditioning before transmission:
    ///
    /// **Baseband Processing Steps:**
    /// 1. **Carrier Frequency Offset (CFO) Correction**: Compensate for oscillator differences
    /// 2. **Amplitude Control**: Ensure proper transmission power levels
    /// 3. **Signal Quality Measurements**: Monitor power, peak levels, clipping
    /// 4. **Buffer Management**: Organize samples for timed transmission
    ///
    /// \code{.cpp}
    /// // Example from downlink_processor_baseband_impl.cpp
    /// bool downlink_processor_baseband_impl::process_new_symbol(baseband_gateway_buffer_writer& buffer, slot_point slot, unsigned i_symbol)
    /// {
    ///     // Perform OFDM modulation  
    ///     bool processed = pdxch_proc_baseband.process_symbol(buffer, pdxch_context);
    ///     
    ///     // Apply RF conditioning to each antenna port
    ///     for (unsigned i_port = 0; i_port != buffer.get_nof_channels(); ++i_port) {
    ///         span<cf_t> channel_buffer = buffer.get_channel_buffer(i_port);
    ///         
    ///         // Apply carrier frequency offset correction
    ///         cfo_processor.process(channel_buffer);
    ///         
    ///         // Apply amplitude control
    ///         amplitude_control.process(channel_buffer, channel_buffer);
    ///         
    ///         // Measure signal quality
    ///         avg_power.update(srsvec::average_power(channel_buffer));
    ///     }
    /// }
    /// \endcode
    void step4_baseband_processing() {
        std::cout << "Step 4: Baseband processor applies final RF conditioning and measurements\n";
    }

    /// \brief Step 5: Baseband Gateway and Radio Transmission
    ///
    /// The baseband gateway interfaces with radio hardware for transmission:
    ///
    /// **Transmission Process:**
    /// 1. **Buffer Management**: Organize samples with precise timestamps
    /// 2. **Hardware Interface**: Send samples to radio backend (UHD/ZMQ)
    /// 3. **Timing Control**: Ensure samples transmitted at exact time
    /// 4. **Multiple Antenna Support**: Handle MIMO transmission streams
    ///
    /// **Radio Hardware Backends:**
    /// - **UHD (USRP Hardware Driver)**: For real USRP radios
    /// - **ZMQ (Zero Message Queue)**: For software-only testing
    /// - **Plugin Architecture**: Support for custom radio backends
    ///
    /// \code{.cpp}
    /// // Example from radio_uhd_baseband_gateway.h
    /// class radio_uhd_baseband_gateway : public baseband_gateway
    /// {
    ///     std::unique_ptr<radio_uhd_tx_stream> tx_stream;  // Transmit to USRP
    ///     std::unique_ptr<radio_uhd_rx_stream> rx_stream;  // Receive from USRP
    ///     
    ///     baseband_gateway_transmitter& get_transmitter() override { return *tx_stream; }
    ///     baseband_gateway_receiver& get_receiver() override { return *rx_stream; }
    /// };
    /// \endcode
    void step5_radio_transmission() {
        std::cout << "Step 5: Baseband gateway sends samples to radio hardware for RF transmission\n";
    }

    /// \brief Complete Signal Flow Summary
    ///
    /// This function demonstrates the complete end-to-end signal flow:
    void demonstrate_complete_signal_flow() {
        std::cout << "\n=== srsRAN Signal Flow: From Main to Baseband Gateway ===\n\n";
        
        step1_application_initialization();
        std::cout << "  ↓ Component connections established\n\n";
        
        step2_upper_phy_signal_generation();  
        std::cout << "  ↓ Resource grids containing 5G signals\n\n";
        
        step3_lower_phy_ofdm_modulation();
        std::cout << "  ↓ Time-domain baseband samples\n\n";
        
        step4_baseband_processing();
        std::cout << "  ↓ RF-conditioned baseband samples\n\n";
        
        step5_radio_transmission();
        std::cout << "  ↓ RF transmission over the air\n\n";
        
        std::cout << "=== Signal Successfully Transmitted! ===\n\n";
        
        std::cout << "Key Points:\n";
        std::cout << "• Real-time processing with strict timing requirements\n";
        std::cout << "• Modular design enables hardware flexibility\n"; 
        std::cout << "• Precise timestamp control ensures synchronized transmission\n";
        std::cout << "• Memory-efficient buffer management and reuse\n";
        std::cout << "• Complete 5G signal processing from L1 to RF\n";
    }
};

} // namespace srsran_signal_flow_example

/// \brief Main function demonstrating the signal flow explanation
///
/// This function can be used as a reference to understand how srsRAN processes
/// signals from application startup to radio transmission.
int main() {
    srsran_signal_flow_example::SignalFlowExplanation explanation;
    explanation.demonstrate_complete_signal_flow();
    return 0;
}