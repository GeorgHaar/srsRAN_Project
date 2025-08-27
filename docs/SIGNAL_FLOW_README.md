# Understanding srsRAN Signal Flow: From Main to Baseband Gateway

This document provides a comprehensive explanation of how srsRAN works internally, tracing the complete signal flow from application startup to radio transmission.

## Quick Overview

srsRAN processes 5G signals through a layered architecture:

```
Main Application → Upper PHY → Lower PHY → Baseband Gateway → Radio Hardware
     ↓               ↓            ↓              ↓               ↓
   Initialize  → Signal Gen → OFDM Modulation → RF Processing → Transmission
```

## Detailed Signal Flow

### 1. Application Entry Points

#### Main gNB Application (`apps/gnb/gnb.cpp`)
- Complete 5G base station implementation
- Initializes all protocol stack layers (L1/L2/L3)
- Sets up CU-CP, CU-UP, and DU components
- Manages network interfaces and radio connections

#### PHY Example (`apps/examples/phy/radio_ssb.cpp`)
- Simplified demonstration of physical layer processing
- Focuses on SSB (Synchronization Signal Block) generation
- Shows direct integration between Upper PHY, Lower PHY, and radio
- **Best starting point** for understanding signal flow

**Key connections in radio_ssb.cpp:**
```cpp
// Create the signal processing chain
std::unique_ptr<radio_session> radio = factory->create(...);           // Radio hardware
std::unique_ptr<lower_phy> lower_phy = create_lower_phy(...);          // OFDM modulation
std::unique_ptr<upper_phy_ssb_example> upper_phy = create(...);        // Signal generation

// Connect the processing chain
rg_gateway_adapter.connect(&lower_phy_instance->get_rg_handler());     // Upper → Lower PHY
radio->start(start_time);                                               // Start radio
lower_phy_instance->get_controller().start(start_time);                // Start processing
```

### 2. Upper PHY Layer - Signal Generation

The Upper PHY generates 5G signals and maps them to frequency-domain resource grids.

#### Responsibilities:
- **Channel Coding**: LDPC encoding, rate matching
- **Scrambling**: Apply pseudo-random sequences  
- **Modulation Mapping**: Convert bits to symbols (QPSK, QAM16, QAM64, QAM256)
- **Resource Element Mapping**: Place symbols in time-frequency grid

#### Key Files:
- `lib/phy/upper/channel_processors/pdcch/pdcch_modulator_impl.cpp` - Control channel processing
- `lib/phy/upper/channel_processors/` - Various signal type processors
- `apps/examples/phy/upper_phy_ssb_example.h` - SSB generation example

#### Example Signal Generation:
```cpp
void pdcch_modulator_impl::modulate(resource_grid_writer& grid, span<const uint8_t> data, const config_t& config)
{
    // 1. Apply scrambling to input data
    scramble(b_hat, data, config);
    
    // 2. Apply QPSK modulation mapping  
    modulate(d_pdcch.get_slice(0), b_hat, config.scaling);
    
    // 3. Map modulated symbols to resource grid
    map(grid, d_pdcch, config);
}
```

**Output**: Resource grids containing frequency-domain 5G signals

### 3. Lower PHY Layer - OFDM Modulation

The Lower PHY converts frequency-domain signals to time-domain baseband samples.

#### Core Process - OFDM Modulation (`lib/phy/lower/modulation/ofdm_modulator_impl.cpp`):

```cpp
void ofdm_symbol_modulator_impl::modulate(span<cf_t> output, const resource_grid_reader& grid, ...)
{
    // 1. Extract frequency-domain data from resource grid
    grid.get(dft->get_input().last(rg_size / 2), ...);   // Negative frequencies
    grid.get(dft->get_input().first(rg_size / 2), ...);  // Positive frequencies
    
    // 2. Perform IFFT to convert to time domain
    span<const cf_t> dft_output = dft->run();
    
    // 3. Apply phase compensation and scaling
    cf_t phase_compensation = phase_compensation_table.get_coefficient(symbol_index);
    srsvec::sc_prod(dft_output, phase_compensation * scale, output.last(dft_size));
    
    // 4. Add cyclic prefix for multipath protection
    srsvec::copy(output.first(cp_len), output.last(cp_len));
}
```

#### Processing Chain (`lib/phy/lower/processors/downlink/pdxch/pdxch_processor_impl.cpp`):

```cpp
bool pdxch_processor_impl::process_symbol(baseband_gateway_buffer_writer& samples, ...)
{
    // Get resource grid from Upper PHY
    auto request = requests.exchange({context.slot, shared_resource_grid()});
    current_grid = std::move(request.grid);
    
    // Modulate each antenna port
    for (unsigned i_port = 0; i_port != nof_tx_ports; ++i_port) {
        // **KEY STEP**: OFDM modulation converts frequency → time domain
        modulator->modulate(samples.get_channel_buffer(i_port), current_grid.get_reader(), i_port, symbol_index);
    }
}
```

**Output**: Time-domain baseband samples

### 4. Baseband Processing - RF Conditioning

Final processing before transmission (`lib/phy/lower/processors/downlink/downlink_processor_baseband_impl.cpp`):

```cpp
bool downlink_processor_baseband_impl::process_new_symbol(baseband_gateway_buffer_writer& buffer, ...)
{
    // 1. Perform OFDM modulation
    bool processed = pdxch_proc_baseband.process_symbol(buffer, pdxch_context);
    
    // 2. Apply RF conditioning to each antenna port
    for (unsigned i_port = 0; i_port != buffer.get_nof_channels(); ++i_port) {
        span<cf_t> channel_buffer = buffer.get_channel_buffer(i_port);
        
        // Carrier frequency offset correction
        cfo_processor.process(channel_buffer);
        
        // Amplitude control for proper transmission power
        amplitude_control.process(channel_buffer, channel_buffer);
        
        // Signal quality measurements
        avg_power.update(srsvec::average_power(channel_buffer));
        peak_power.update(srsvec::max_abs_element(channel_buffer).second);
    }
}
```

#### Processing Steps:
1. **Carrier Frequency Offset (CFO) Correction**: Compensate for oscillator differences
2. **Amplitude Control**: Ensure proper transmission power levels  
3. **Signal Quality Measurements**: Monitor power, clipping, signal integrity
4. **Buffer Management**: Organize samples with precise timestamps

**Output**: RF-ready baseband samples

### 5. Baseband Gateway - Radio Interface

The Baseband Gateway provides the interface between PHY processing and radio hardware.

#### Buffer Management (`lib/phy/lower/processors/downlink/downlink_processor_baseband_impl.h`):

```cpp
class baseband_symbol_buffer
{
    baseband_gateway_buffer_dynamic buffer;           // Sample storage
    baseband_gateway_timestamp symbol_start_timestamp; // Timing information
    
    // Write samples with precise timing
    baseband_gateway_buffer_writer& write_symbol(baseband_gateway_timestamp timestamp, unsigned size);
    
    // Read samples at specific timestamp
    unsigned read(baseband_gateway_buffer_writer& out, baseband_gateway_timestamp timestamp);
};
```

#### Transmission Metadata (`include/srsran/gateways/baseband/baseband_gateway_transmitter_metadata.h`):

```cpp
struct baseband_gateway_transmitter_metadata {
    baseband_gateway_timestamp ts;          // Exact transmission time
    bool is_empty;                          // Empty buffer indicator
    std::optional<unsigned> tx_start;       // Start of valid signal
    std::optional<unsigned> tx_end;         // End of valid signal
};
```

### 6. Radio Hardware Interface

Final transmission through hardware backends.

#### UHD Backend (`lib/radio/uhd/radio_uhd_baseband_gateway.h`):
```cpp
class radio_uhd_baseband_gateway : public baseband_gateway
{
    std::unique_ptr<radio_uhd_tx_stream> tx_stream;  // USRP transmit stream
    std::unique_ptr<radio_uhd_rx_stream> rx_stream;  // USRP receive stream
    
    baseband_gateway_transmitter& get_transmitter() override { return *tx_stream; }
    baseband_gateway_receiver& get_receiver() override { return *rx_stream; }
};
```

#### Supported Radio Backends:
- **UHD (USRP Hardware Driver)**: Real USRP radios (B200, N300, X300, etc.)
- **ZMQ (Zero Message Queue)**: Software-only testing and simulation
- **Plugin Architecture**: Custom radio backend support

## Complete Signal Flow Summary

1. **Application Startup**: Initialize components and establish connections
2. **Signal Generation**: Upper PHY creates 5G signals in resource grids
3. **OFDM Modulation**: Lower PHY converts frequency → time domain
4. **RF Conditioning**: Apply CFO correction, amplitude control, measurements
5. **Buffer Management**: Organize samples with precise timestamps
6. **Hardware Transmission**: Send to radio hardware (USRP/ZMQ) for RF transmission

## Key Design Principles

- **Real-time Processing**: Strict timing deadlines for 5G requirements
- **Modular Architecture**: Clear separation between processing layers  
- **Hardware Abstraction**: Interchangeable radio backends
- **Precise Timing**: Timestamp-driven sample processing
- **Memory Efficiency**: Buffer reuse and pool management
- **MIMO Support**: Multiple antenna streams processing

## Files Modified with Documentation

To help understand the signal flow, the following files have been enhanced with detailed comments:

1. **`apps/examples/phy/radio_ssb.cpp`**: Added comprehensive signal flow documentation showing component connections and processing loop
2. **`lib/phy/lower/modulation/ofdm_modulator_impl.cpp`**: Detailed OFDM modulation process explanation
3. **`lib/phy/lower/processors/downlink/pdxch/pdxch_processor_impl.cpp`**: Resource grid to baseband sample conversion
4. **`lib/phy/lower/processors/downlink/downlink_processor_baseband_impl.cpp`**: Final RF conditioning before transmission

## Additional Documentation

- **`docs/srsran_signal_flow_architecture.md`**: Comprehensive technical documentation
- **`docs/signal_flow_example.cpp`**: Executable example demonstrating the concepts

## Getting Started

1. **Read the Code**: Start with `apps/examples/phy/radio_ssb.cpp` to see the complete flow
2. **Follow the Data**: Trace how resource grids become baseband samples
3. **Understand Timing**: See how precise timestamps control the entire process
4. **Explore Components**: Examine each layer's responsibilities and interfaces

This architecture enables srsRAN to provide a complete, high-performance 5G RAN implementation while maintaining the strict real-time requirements of 5G systems.