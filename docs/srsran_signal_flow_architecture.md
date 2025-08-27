# srsRAN Signal Flow Architecture: From Main to Baseband Gateway

This document explains how srsRAN works internally, tracing the signal flow from the main application entry point through signal generation, modulation, and transmission to the baseband gateway.

## Overview

srsRAN implements a complete 5G RAN solution with a layered architecture that processes signals from high-level 5G protocols down to baseband samples that are transmitted over radio hardware. The signal flow follows this path:

```
Main Application → Upper PHY → Lower PHY → Baseband Gateway → Radio Hardware
     ↓               ↓            ↓              ↓               ↓
   gNB/Example → Signal Gen → OFDM Modulation → Sample Buffers → UHD/ZMQ
```

## 1. Main Application Entry Points

### 1.1 Full gNB Application (`apps/gnb/gnb.cpp`)
The main gNB application is the complete 5G base station implementation:
- Initializes all protocol stack layers (L1/L2/L3)
- Sets up CU-CP, CU-UP, and DU components
- Configures radio interfaces and baseband processing
- Manages network interfaces and protocols

### 1.2 PHY Examples (`apps/examples/phy/radio_ssb.cpp`)
The radio_ssb example demonstrates the core signal processing flow:
- Simplified application focusing on physical layer
- Generates Synchronization Signal Blocks (SSB)
- Shows direct integration between upper PHY, lower PHY, and radio
- Useful for understanding the fundamental signal processing chain

**Key Components in radio_ssb.cpp:**
```cpp
// Create radio hardware interface
std::unique_ptr<radio_session> radio = factory->create(radio_config, *async_task_executor, notification_handler);

// Create lower physical layer (modulation, baseband processing)
std::unique_ptr<lower_phy> lower_phy_instance = create_lower_phy(phy_config);

// Create upper physical layer (signal generation)
std::unique_ptr<upper_phy_ssb_example> upper_phy = upper_phy_ssb_example::create(upper_phy_sample_config);

// Connect the layers together
rg_gateway_adapter.connect(&lower_phy_instance->get_rg_handler());

// Start processing
radio->start(start_time);
lower_phy_instance->get_controller().start(start_time);
```

## 2. Upper PHY Layer - Signal Generation

The Upper PHY layer is responsible for generating the actual 5G signals and symbols that will be transmitted.

### 2.1 Signal Processing Functions
- **Channel Coding**: LDPC encoding, rate matching
- **Modulation Mapping**: QPSK, QAM16, QAM64, QAM256
- **Channel Processing**: PDCCH, PDSCH, SSB generation
- **Resource Grid Creation**: Maps symbols to time-frequency resources

### 2.2 Key Components
- **Channel Processors** (`lib/phy/upper/channel_processors/`):
  - PDCCH modulator (`pdcch_modulator_impl.cpp`)
  - PDSCH processor
  - SSB processor
- **Resource Grid**: Time-frequency domain representation of signals
- **Schedulers**: Determine what signals to transmit when

### 2.3 Signal Generation Flow
```cpp
// Example from pdcch_modulator_impl.cpp
void pdcch_modulator_impl::modulate(resource_grid_writer& grid, span<const uint8_t> data, const pdcch_modulator::config_t& config)
{
    // 1. Apply scrambling to input data
    scramble(b_hat, data, config);
    
    // 2. Apply modulation mapping (QPSK typically for PDCCH)
    modulate(d_pdcch.get_slice(0), b_hat, config.scaling);
    
    // 3. Map modulated symbols to resource grid
    map(grid, d_pdcch, config);
}
```

## 3. Lower PHY Layer - OFDM Modulation

The Lower PHY layer takes the resource grid from Upper PHY and converts it to time-domain baseband samples.

### 3.1 OFDM Modulation Process
The core of the lower PHY is OFDM (Orthogonal Frequency Division Multiplexing) modulation:

**Key file**: `lib/phy/lower/modulation/ofdm_modulator_impl.cpp`

```cpp
void ofdm_symbol_modulator_impl::modulate(span<cf_t> output, const resource_grid_reader& grid, unsigned port_index, unsigned symbol_index)
{
    // 1. Extract frequency domain data from resource grid
    grid.get(dft->get_input().last(rg_size / 2), port_index, symbol_index % nsymb, 0);
    grid.get(dft->get_input().first(rg_size / 2), port_index, symbol_index % nsymb, rg_size / 2);
    
    // 2. Perform IFFT to convert to time domain
    span<const cf_t> dft_output = dft->run();
    
    // 3. Apply phase compensation and scaling
    cf_t phase_compensation = phase_compensation_table.get_coefficient(symbol_index);
    srsvec::sc_prod(dft_output, phase_compensation * scale, output.last(dft_size));
    
    // 4. Add cyclic prefix
    srsvec::copy(output.first(cp_len), output.last(cp_len));
}
```

### 3.2 Downlink Processing Chain
**Key file**: `lib/phy/lower/processors/downlink/pdxch/pdxch_processor_impl.cpp`

The downlink processor coordinates between resource grids and modulated output:

```cpp
bool pdxch_processor_impl::process_symbol(baseband_gateway_buffer_writer& samples, const pdxch_processor_baseband::symbol_context& context)
{
    // 1. Get the resource grid for current slot
    if (context.slot != current_slot) {
        auto request = requests.exchange({context.slot, shared_resource_grid()});
        current_grid = std::move(request.grid);
    }
    
    // 2. Modulate each antenna port
    for (unsigned i_port = 0; i_port != nof_tx_ports; ++i_port) {
        modulator->modulate(samples.get_channel_buffer(i_port), current_grid.get_reader(), i_port, symbol_index_subframe);
    }
    
    return true;
}
```

### 3.3 Baseband Processing
**Key file**: `lib/phy/lower/processors/downlink/downlink_processor_baseband_impl.cpp`

```cpp
bool downlink_processor_baseband_impl::process_new_symbol(baseband_gateway_buffer_writer& buffer, slot_point slot, unsigned i_symbol)
{
    // 1. Process symbol through PDxCH processor (gets modulated samples)
    bool processed = pdxch_proc_baseband.process_symbol(buffer, pdxch_context);
    
    // 2. Apply post-processing to baseband samples
    for (unsigned i_port = 0; i_port != buffer.get_nof_channels(); ++i_port) {
        span<cf_t> channel_buffer = buffer.get_channel_buffer(i_port);
        
        // Apply carrier frequency offset
        cfo_processor.process(channel_buffer);
        
        // Process amplitude control  
        amplitude_control.process(channel_buffer, channel_buffer);
        
        // Perform signal measurements
        avg_power.update(srsvec::average_power(channel_buffer));
        peak_power.update(srsvec::max_abs_element(channel_buffer).second);
    }
}
```

## 4. Baseband Gateway Interface

The Baseband Gateway provides the interface between the PHY processing and the actual radio hardware.

### 4.1 Baseband Buffer Management
**Key file**: `lib/phy/lower/processors/downlink/downlink_processor_baseband_impl.h`

```cpp
class baseband_symbol_buffer
{
    // Stores baseband samples for transmission
    baseband_gateway_buffer_dynamic buffer;
    baseband_gateway_timestamp symbol_start_timestamp;
    
    // Write samples into buffer with timestamp
    baseband_gateway_buffer_writer& write_symbol(baseband_gateway_timestamp symbol_timestamp, unsigned symbol_size);
    
    // Read samples from buffer at specific timestamp  
    unsigned read(baseband_gateway_buffer_writer& out, baseband_gateway_timestamp timestamp);
};
```

### 4.2 Transmission Metadata
**Key file**: `include/srsran/gateways/baseband/baseband_gateway_transmitter_metadata.h`

```cpp
struct baseband_gateway_transmitter_metadata {
    baseband_gateway_timestamp ts;          // When to transmit
    bool is_empty;                          // Empty buffer flag
    std::optional<unsigned> tx_start;       // Start of signal in buffer
    std::optional<unsigned> tx_end;         // End of signal in buffer
};
```

## 5. Radio Hardware Interface

The final stage sends baseband samples to actual radio hardware through various backends.

### 5.1 UHD (USRP Hardware Driver)
**Key file**: `lib/radio/uhd/radio_uhd_baseband_gateway.h`

```cpp
class radio_uhd_baseband_gateway : public baseband_gateway
{
    std::unique_ptr<radio_uhd_tx_stream> tx_stream;  // Transmit stream to USRP
    std::unique_ptr<radio_uhd_rx_stream> rx_stream;  // Receive stream from USRP
    
    baseband_gateway_transmitter& get_transmitter() override { return *tx_stream; }
    baseband_gateway_receiver& get_receiver() override { return *rx_stream; }
};
```

### 5.2 ZMQ (Zero Message Queue - for testing)
ZMQ backend allows software-only testing without real hardware.

## 6. Complete Signal Flow Summary

1. **Application Start**: Main function initializes all components and connections
2. **Signal Generation**: Upper PHY creates symbols and maps them to resource grids
3. **OFDM Modulation**: Lower PHY converts frequency-domain symbols to time-domain samples
4. **Baseband Processing**: Apply final processing (CFO, amplitude control, measurements)
5. **Buffer Management**: Store samples in timestamped buffers for transmission
6. **Hardware Transmission**: Send samples to radio hardware (USRP, etc.) at precise timing

The architecture uses event-driven processing with precise timing control to ensure samples are generated and transmitted at exactly the right time for proper 5G operation.

## Key Design Principles

- **Real-time Processing**: All stages must complete within strict timing deadlines
- **Modular Design**: Clear separation between signal processing layers
- **Hardware Abstraction**: Radio backends are interchangeable (UHD, ZMQ, etc.)
- **Precise Timing**: Timestamp-driven sample processing for synchronized transmission
- **Memory Efficiency**: Reuse of buffers and pools to minimize allocation overhead

This architecture enables srsRAN to provide a complete, high-performance 5G RAN implementation that can run on various hardware platforms while maintaining the strict timing requirements of 5G systems.