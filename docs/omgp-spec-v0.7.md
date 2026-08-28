<!-- Extracted from OMGP_Specification_v0_7.docx (pandoc, gfm).
     Module example names are generic by policy: no third-party trademarks
     appear anywhere in this public repository. -->

**OMGP**

**Open Modular Guitar Platform**

Hardware, Software, Backplane & Development Specification

**Draft v0.7**

Advanced DIY / Open Hardware

This specification is deliberately pre-production. Electrical limits, connector selections, high-voltage arrangements and safety requirements must be validated by calculation, bench testing and independent review before construction of user-accessible high-voltage hardware.

# Document Control

| **Field**                | **Value**                                                                                |
|--------------------------|------------------------------------------------------------------------------------------|
| Document                 | Open Modular Guitar Platform — Hardware, Software, Backplane & Development Specification |
| Version                  | 0.7                                                                                      |
| Status                   | Pre-prototype architecture and interface draft                                           |
| Target community         | Advanced DIY builders, open-hardware developers, pedal/amp designers                     |
| Reference host           | ESP32-S3                                                                                 |
| Reference module tooling | Arduino-compatible C++ / ESP-IDF                                                         |
| System control bus       | Two-tier: I2C/SMBus module bus per backplane; RS-485 host trunk (1 Mbit/s reference)     |
| Audio philosophy         | Analogue-first; optional DSP; the control buses never carry normal audio                 |

# 1. Vision and Scope

OMGP is an open hardware and software platform for building modular guitar signal-processing systems from interchangeable effects, preamplifiers, routing, simulation, output and control modules. The platform is intended to scale from a compact DIY host to large rack systems with multiple effect and preamp backplanes.

**Core principle:** the platform defines interoperability; the module designer defines the sound.

- OMGP targets rack and studio use. Modules are control-less signal hardware: all user interaction occurs through host control surfaces, MIDI remote control, native OMGP control modules and the desktop application.

Traditional analogue effects and pickup-sensitive circuits remain possible.

- Real high-voltage tube preamps are first-class modules.

- Any module may expose multiple switchable channels, voices or major operating configurations.

- Module capacity is expandable through intelligent FX and preamp backplanes.

- The raw analogue path can drive external guitar power amplifiers hard enough for genuine downstream power-stage behaviour.

- A separate simulated path supports amp/cab simulation, line outputs and headphones.

- The system remains usable without a PC, cloud service or internet connection.

# 2. Design Principles

| **Principle**                   | **Requirement**                                                                                                                                                |
|---------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Open                            | Electrical, mechanical, protocol, reference PCB and firmware information is publicly documented.                                                               |
| Advanced-DIY friendly           | Reference designs use familiar tools, readily sourced components and Arduino-compatible development where practical.                                           |
| Modular                         | Effects, preamps, routing, power, simulation, outputs and control are separable modules.                                                                       |
| Expandable                      | FX and preamp capacity can grow through additional intelligent backplanes.                                                                                     |
| Analogue-first                  | Digital control does not require digital audio.                                                                                                                |
| Tube capable                    | Real ECC83/12AX7, 6N2P/6Н2П and other suitable preamp-valve designs are supported.                                                                             |
| MCU independent                 | Compatibility is defined by interface behaviour, not a mandatory processor.                                                                                    |
| Host-controlled                 | Modules carry no mandatory front-panel controls; the declared parameter model is the sole control path, exposed via host UI, MIDI and the desktop application. |
| Preserve intentional distortion | The host must not automatically remove intentional inter-stage or power-amp overdrive.                                                                         |
| Fail safely                     | Software supervises safety; safety-critical protection is hardware-backed.                                                                                     |

# 3. System Architecture

OMGP HOST  
ESP32-S3  
\|  
RS-485 TRUNK  
\|  
+------------------+-------------------+  
\| \| \|  
v v v  
FX BACKPLANE PREAMP BACKPLANE DSP / OUTPUT  
\| \|  
LOCAL ROUTER LOCAL ROUTER  
\| \|  
FX MODULES PREAMP MODULES  
\| \|  
v v  
FX EXPANSION PREAMP EXPANSION

The architecture is distributed. Backplanes are active nodes with local slot management, power control and, where appropriate, local analogue routing. This avoids a central controller requiring hundreds of GPIO lines and avoids forcing all analogue signals through one enormous central crosspoint. Control communication is two-tier: each backplane controller bridges the RS-485 host trunk to a local I2C/SMBus module bus, so module developers implement only a simple I2C peripheral and never attach to the trunk directly.

# 4. Module and Backplane Classes

| **Class**             | **Typical purpose**                                                             |
|-----------------------|---------------------------------------------------------------------------------|
| Analogue Effect       | Fuzz, overdrive, compressor, boost, EQ, analogue modulation                     |
| Smart Analogue Effect | Analogue audio with MCU-controlled pots, relays, DACs, VCAs or switches         |
| Tube Preamp           | Real tube gain stages, cathode followers, recovery stages and switched channels |
| Solid-State Preamp    | Transistor/op-amp/FET amp-style preamps                                         |
| DSP Effect            | Delay, reverb, modulation, pitch, experimental digital effects                  |
| Hybrid Module         | Mixed analogue/tube/DSP designs                                                 |
| Simulation Module     | Power-amp model, cabinet IR, mic/room simulation                                |
| Routing / Mixer       | Local or global audio routing and parallel mixing                               |
| Output Module         | Raw out, balanced line, headphone, USB audio options                            |
| Control Module        | Encoders, switches, displays, MIDI, expression                                  |
| Power Module          | Low-voltage rails                                                               |
| Tube Power Module     | 6.3 V heater and B+ generation/protection                                       |
| Development Module    | Breakout and prototyping hardware                                               |

# 5. Capacity and Expansion

The OMGP protocol shall support at least 64 addressable nodes/modules across all backplane segments. Physical capacity is set by the host implementation rather than a small protocol-level limit.

- Reference FX backplane: typically 8–16 effect positions; additional FX backplanes may be added.

- Reference preamp backplane: up to 4 physical preamp positions; additional preamp backplanes may be added.

- A system may therefore contain many effects and more than four preamps in total.

- Each preamp backplane may provide its own tube-power capability, avoiding distribution of a very large central B+ supply.

Example expanded system  
  
FX Bank A: 12 effects  
Preamp Bank A: 4 preamps  
FX Bank B: 12 effects  
Preamp Bank B: 4 preamps  
DSP/Sim: 1 or more modules  
  
All remain part of the same OMGP control/preset environment.

# 6. Universal Module Channel Model

Channels are a generic OMGP concept and are available to any module type. A channel represents a major selectable signal configuration such as Clean/Crunch/Lead, Vintage/Modern/Boost or Tape/Digital/Analog.

MODULE  
\|-- Channel 0  
\|-- Channel 1  
\|-- Channel 2  
\|-- Module-wide parameters  
\`-- Channel-specific parameters

Channel count is declared dynamically. Modules without a meaningful multi-channel concept may expose one default channel.

| **Example module** | **Possible channels**                             |
|--------------------|---------------------------------------------------|
| Tube preamp        | Clean / Crunch / Lead                             |
| Overdrive          | Vintage / Modern / Boost                          |
| Fuzz               | Germanium / Silicon / Octave                      |
| Delay              | Tape / Digital / Analog                           |
| DSP multi-effect   | Developer-defined algorithms or signal topologies |

# 7. Channel and Parameter Control

The host sends abstract commands. Module-local firmware performs the hardware-specific operation.

HOST  
SELECT_CHANNEL = 2  
\|  
v  
MODULE BUS  
\|  
v  
MODULE MCU  
mute if required  
switch relays/FETs/analogue switches  
recall digital controls  
wait for settling  
unmute

- Channel selection is distinct from module bypass.

- Parameters may exist at module scope or channel scope.

- Supported parameter types include continuous, toggle, enumeration, momentary, trigger and read-only telemetry.

- Reference continuous logical resolution is 0–4095.

- Modules may declare switching time, whether muting is required and whether switching is seamless.

# 8. Preset Model

A global preset may contain the complete rig state:

- Active modules and bypass states

- Selected module channels

- Module-scoped and channel-scoped parameter values

- Audio routing and module order

- Parallel/wet-dry configuration

- Simulation and cabinet IR selection

- Raw, simulated and headphone output levels

- Expression mappings

- MIDI mappings

PRESET: LEAD  
  
Preamp P3  
Channel = Lead  
Gain = 82%  
Master = 41%  
  
Green Overdrive  
Channel = Vintage  
Drive = 28%  
Level = 76%  
Enabled = true  
  
Delay  
Channel = Tape  
Time = 420 ms  
Mix = 22%  
  
Cab = 4x12 V30

# 9. Audio Philosophy and Signal Levels

OMGP distinguishes the operating level inside a module from the capability of the interconnect/backplane. A vintage 9 V effect does not have to be redesigned to operate at the same level as a high-output tube preamp.

| **Parameter**                        | **Provisional target**                                |
|--------------------------------------|-------------------------------------------------------|
| Guitar input impedance               | 1 MΩ nominal                                          |
| Preferred buffered module input      | ≥500 kΩ; 1 MΩ preferred for pedal/preamp-style inputs |
| Preferred module output impedance    | \<1 kΩ; \<100 Ω for buffered outputs                  |
| Reference nominal interconnect level | 1 Vrms                                                |
| Standard bus capability              | ≥4 Vrms                                               |
| High-headroom capability             | ≥8 Vrms                                               |
| Routing design target                | ~10 Vrms clean where practical                        |
| Raw power-amp output                 | 8–10 Vrms clean target                                |
| Raw output impedance                 | \<100 Ω                                               |

Automatic loudness normalisation is not mandatory and shall not silently prevent a boost, preamp or effect from intentionally overdriving the following stage.

# 10. Pickup-Sensitive Modules

Certain fuzz, treble-booster, wah and other vintage circuits depend on the electrical interaction with passive guitar pickups. Modules may declare INPUT_MODE=PICKUP_SENSITIVE. Host routing should avoid an inappropriate buffer ahead of such a module where the physical topology can support it.

# 11. Optional Inter-Module Attenuation

The routing architecture should investigate approximately 0 to -24 dB digitally controlled attenuation ahead of selected module inputs. Its default state is 0 dB.

High-output tube preamp  
\|  
10 Vrms  
\|  
optional attenuator  
\|  
low-headroom 9 V effect

Attenuation may protect or correctly level a low-headroom module, but the user/preset must retain the ability to deliberately overdrive downstream circuitry.

# 12. Audio Routing Architecture

Modules are routable nodes. Slot order does not inherently determine signal order. The system should support reorder, bypass, FX-loop insertion and eventually parallel/wet-dry routing.

Possible chain:  
Guitar -\> Compressor -\> Green Overdrive -\> Preamp 2/Lead -\> Chorus -\> Delay -\> Raw/Sim  
  
Possible parallel chain:  
+-\> Preamp 1 -\> FX ----+  
Guitar ------+ +-\> Mix  
+-\> Preamp 2 -\> FX ----+

# 13. Global Audio Fabric

Expanded systems should use local routing on each backplane and a smaller global audio fabric between backplanes. The architecture should provision for up to eight global analogue buses where practical; an early implementation may initially populate four.

- Supports stereo paths.

- Allows parallel and wet/dry paths.

- Avoids a giant 64-module central analogue crosspoint.

- Permits multiple output/simulation paths.

# 14. Transparent Host Audio Targets

| **Metric**                  | **Provisional target**                                |
|-----------------------------|-------------------------------------------------------|
| Frequency response          | 20 Hz–20 kHz ±0.2 dB                                  |
| THD+N                       | \<0.01% at 1 kHz / 1 Vrms for transparent host stages |
| Equivalent host noise floor | ≤ -90 dBu target                                      |
| Crosstalk                   | ≤ -80 dB target                                       |
| Buffered output impedance   | \<100 Ω                                               |

# 15. Power Rails

| **Rail**    | **Purpose**                                | **Provisional per-slot target** |
|-------------|--------------------------------------------|---------------------------------|
| +15 V       | High-headroom analogue                     | 150 mA                          |
| -15 V       | High-headroom analogue                     | 150 mA                          |
| +9 V        | Traditional pedal circuits                 | 500 mA                          |
| +5 V        | Digital/DSP/peripheral power               | 1 A                             |
| +3.3 V STBY | Discovery, MCU, module bus, identification | ~100 mA                         |

System-wide budgets may limit simultaneous maximum loading. Modules must locally decouple and filter their supplies.

# 16. Slot Power Management

CARD PRESENT  
\|  
+3V3_STBY  
\|  
module controller starts  
\|  
descriptor over I2C  
\|  
validate requested resources  
\|  
enable approved rails  
\|  
MODULE_ENABLE

- Each slot should use independent protected power switching/eFuse functionality.

- A faulty development card should not collapse the complete chassis.

- Backplanes should report current, power-good and fault status where practical.

# 17. Tube Module Scope

OMGP v1 supports real preamp tubes but does not attempt to power output valves. A single tube-preamp module may contain up to four dual-triode valves (eight triode sections) within its declared power envelope.

- Reference families include 12AX7/ECC83 and 6N2P/6Н2П.

- Other suitable preamp valves may be used when module wiring and electrical requirements are correct.

- Eight triode sections are not assumed to be eight voltage-gain stages; sections may be followers, recovery stages, mixers or other functions.

# 18. Tube Power Classes

| **Class** | **Typical configuration**   | **6.3 V heater allocation** | **B+ allocation** |
|-----------|-----------------------------|-----------------------------|-------------------|
| T1        | 1 dual triode               | 0.5 A                       | 8 mA              |
| T2        | 2 dual triodes              | 1.0 A                       | 15 mA             |
| T3        | 3 dual triodes              | 1.5 A                       | 25 mA             |
| T4        | 4 dual triodes / 8 sections | 2.0 A                       | 35 mA             |

T1–T4 are compatibility/allocation classes. The host reserves power using the module's declared maximum requirements. Initial v1 maximum B+ allocation for one module is 40 mA.

# 19. Tube Supply Targets

| **Parameter**                                    | **Reference target** |
|--------------------------------------------------|----------------------|
| Heater supply                                    | 6.3 V DC             |
| B+ nominal                                       | ~250 V DC            |
| B+ architectural range                           | ~180–300 V DC        |
| Reference 4-position preamp-backplane heater PSU | ~6.3 V / 5 A         |
| Reference 4-position preamp-backplane B+ PSU     | ~250 V / 100 mA      |

These are prototype design targets, not frozen safety ratings. Each preamp backplane may use a local tube PSU. Heater and B+ capacity are tracked independently.

# 20. Tube Power Declaration

TYPE = TUBE_PREAMP  
TUBES = 4  
TRIODE_SECTIONS = 8  
  
HEATER_VOLTAGE = 6.3V  
HEATER_CURRENT_NOMINAL = 1.20A  
HEATER_CURRENT_MAX = 1.35A  
  
BPLUS_NOMINAL = 250V  
BPLUS_CURRENT_EXPECTED = 17mA  
BPLUS_CURRENT_MAX = 24mA  
  
POWER_CLASS = T4

# 21. Tube Startup, Standby and Switching

Installed tube preamps may keep heaters energised when adequate power is available so that switching between preamps or channels does not require repeated warm-up delays. B+, mute and routing may be managed independently.

MODULE DETECT  
\|  
validate requirements  
\|  
enable heater  
\|  
verify heater current  
\|  
warm-up  
\|  
enable B+  
\|  
verify B+ current  
\|  
enable audio

# 22. Tube Shutdown and Safety

MUTE AUDIO  
\|  
DISABLE B+  
\|  
DISCHARGE HV  
\|  
VERIFY SAFE VOLTAGE  
\|  
DISABLE HEATER  
\|  
SAFE-TO-REMOVE

OMGP v1 tube modules are not hot-swappable. Firmware is supervisory only: overcurrent, overvoltage, thermal, interlock and stored-energy protection must have appropriate hardware mechanisms. B+ shall never appear on the ordinary low-voltage module connector.

# 23. Standard Low-Voltage Module Connector

The current prototype interface is a logical 56-contact, 2×28 card-edge interface. A 2.54 mm-class DIY-friendly connector and 1.6 mm PCB edge are the starting mechanical assumptions, but the exact manufacturer/part remains Alpha.

# 24. Connector Pinout — Row A

| **Pin** | **Signal**     | **Purpose**                                  |
|---------|----------------|----------------------------------------------|
| A1      | GND            | First/last ground                            |
| A2      | GND            | Ground                                       |
| A3      | +3V3_STBY      | Discovery/controller power                   |
| A4      | +3V3_STBY      | Additional contact                           |
| A5      | GND            | Digital return                               |
| A6      | I2C_SDA        | Module bus data (SMBus)                      |
| A7      | I2C_SCL        | Module bus clock (SMBus)                     |
| A8      | GND            | Module-bus reference/guard                   |
| A9      | SLOT_PRESENT#  | Hardware presence; module pulls low          |
| A10     | MODULE_ENABLE  | Host permission for normal operation         |
| A11     | RESET#         | Optional reset                               |
| A12     | IRQ#           | Optional module fault/event (SMBALERT-style) |
| A13     | GND            | Guard                                        |
| A14     | SYNC           | Reserved synchronisation/trigger             |
| A15     | AUX_DIGITAL_0  | Expansion                                    |
| A16     | AUX_DIGITAL_1  | Expansion                                    |
| A17     | GND            | Guard                                        |
| A18     | +5V            | Main digital power                           |
| A19     | +5V            | Main digital power                           |
| A20     | GND            | Power return                                 |
| A21     | +9V            | Legacy analogue power                        |
| A22     | +9V            | Legacy analogue power                        |
| A23     | GND            | Power return                                 |
| A24     | RESERVED       | Future                                       |
| A25     | RESERVED       | Future                                       |
| A26     | GND            | Guard                                        |
| A27     | CHASSIS/SHIELD | Optional shield reference                    |
| A28     | GND            | First/last ground                            |

# 25. Connector Pinout — Row B

| **Pin** | **Signal**     | **Purpose**               |
|---------|----------------|---------------------------|
| B1      | GND            | First/last ground         |
| B2      | +15V           | Analogue rail             |
| B3      | +15V           | Analogue rail             |
| B4      | AGND           | Analogue return           |
| B5      | -15V           | Analogue rail             |
| B6      | -15V           | Analogue rail             |
| B7      | AGND           | Analogue return           |
| B8      | AUDIO_IN_L     | Primary input             |
| B9      | AGND           | Audio guard/reference     |
| B10     | AUDIO_OUT_L    | Primary output            |
| B11     | AGND           | Audio guard/reference     |
| B12     | AUDIO_IN_R     | Stereo input              |
| B13     | AGND           | Audio guard/reference     |
| B14     | AUDIO_OUT_R    | Stereo output             |
| B15     | AGND           | Audio guard/reference     |
| B16     | AUDIO_SENSE    | Reserved sensing/function |
| B17     | AGND           | Guard                     |
| B18     | AUDIO_AUX_1    | Future audio/CV/sidechain |
| B19     | AGND           | Guard                     |
| B20     | AUDIO_AUX_2    | Future audio/CV/sidechain |
| B21     | AGND           | Guard                     |
| B22     | SLOT_ID_0      | Physical slot ID          |
| B23     | SLOT_ID_1      | Physical slot ID          |
| B24     | SLOT_ID_2      | Physical slot ID          |
| B25     | GND            | Guard                     |
| B26     | RESERVED       | Future                    |
| B27     | CHASSIS/SHIELD | Optional shield reference |
| B28     | GND            | First/last ground         |

# 26. Audio Signalling on Module Edge

The v1 prototype uses single-ended module audio referenced to AGND. This is simpler for DIY effects and appropriate for short traces inside a common chassis. Balanced/differential external outputs remain an output-module concern.

Mono module:  
B8 AUDIO_IN_L  
B9 AGND  
B10 AUDIO_OUT_L  
B11 AGND  
  
Stereo module additionally:  
B12 AUDIO_IN_R  
B13 AGND  
B14 AUDIO_OUT_R  
B15 AGND

# 27. Tube Power Connector

Tube modules use a separate keyed, recessed, HV-rated connector providing:

B+  
HV_RETURN  
HEATER+  
HEATER-  
HV_INTERLOCK  
HV_SENSE

The physical connector is intentionally not frozen until voltage, current, creepage, insertion and touch-safety requirements have been validated.

# 28. Control Bus Architecture

Control communication is two-tier. Modules never attach to the host trunk; each backplane controller bridges between the two layers and supervises its local module bus.

Host trunk (backplane-to-host):

- RS-485 (TIA/EIA-485), half-duplex, host-polled master/slave.

- Reference rate: 1 Mbit/s. Node count is small and fixed: host plus backplane controllers and optional native control surfaces.

- 120 Ω termination at both physical ends of the trunk; failsafe biasing provided on the host or backplane, never on removable nodes.

- Framed link protocol with addressing, CRC-16, defined turnaround timing, retry semantics and a deterministic poll schedule. This link-layer specification is a first-class protocol document.

- Backplane layout should preserve a controlled trunk with short stubs.

Module bus (module-to-backplane, per backplane segment):

- I2C at 400 kHz using SMBus conventions: Packet Error Checking (CRC-8), clock-low timeouts and alert signalling via IRQ# (SMBALERT-style).

- Module address derives from SLOT_ID_0-2 plus a backplane-assigned base; modules never require address jumpers.

- Segments are short and local to one backplane, limiting bus capacitance and confining electrical faults.

- The backplane controller may power-cycle an individual slot to recover a hung device; a faulty module must not take down other backplanes or the trunk.

- Every MCU family provides I2C peripheral support, so module developers need no bus transceiver or protocol stack beyond the SDK.

120R 120R  
HOST ==== BP1 ======== BP2 ======== BP3 (RS-485 trunk)  
\| \| \|  
I2C bus I2C bus I2C bus  
\| \| \|  
modules modules modules

# 29. Reference Host MCU

The reference host uses ESP32-S3 because it is inexpensive, widely supported and accessible through Arduino and ESP-IDF. The trunk uses a standard UART plus RS-485 transceiver; no CAN controller is required on any node.

- Module/backplane discovery and orchestration

- Preset storage and recall

- Routing coordination

- Encoders, switches, displays and LEDs

- MIDI and expression

- USB configuration/updates

- Power coordination and diagnostics

- Optional Wi-Fi/Bluetooth features

Wireless services are optional and must be disable-able during normal audio operation. Internet/cloud access is never required.

# 30. MCU Independence and OMGP Core

The protocol may be implemented using ESP32, STM32, RP2040/RP2350, Teensy, AVR, PIC, FPGA, Linux or custom logic. The project should also provide an optional small OMGP Core board for developers.

OMGP CORE  
+-------------------------+  
\| ESP32-class MCU \|  
\| I2C module-bus interface\|  
\| USB programming/debug \|  
\| regulation \|  
\| SPI / I2C / UART \|  
\| ADC / PWM / GPIO \|  
+------------+------------+  
\|  
EFFECT PCB

# 31. Arduino-Compatible Module SDK

\#include \<OMGP.h\>  
  
OMGPModule module("Dual Drive");  
  
void setup() {  
module.begin();  
  
module.addChannel("Vintage");  
module.addChannel("Modern");  
module.addChannel("Boost");  
  
module.addParameter("Drive");  
module.addParameter("Tone");  
module.addParameter("Level");  
}  
  
void loop() {  
module.update();  
}

module.onChannelChange(\[\](uint8_t channel) {  
setHardwareChannel(channel);  
});  
  
module.onParameter("Drive", \[\](uint16_t value) {  
setDrive(value);  
});

The SDK should abstract module-bus (I2C/SMBus) initialisation, discovery, addressing, heartbeats, parameter transport, protocol versioning and routine error handling.

# 32. Module Discovery Descriptor

NAME = "British 4"  
MANUFACTURER = "DIY Builder"  
TYPE = TUBE_PREAMP  
PROTOCOL = 1  
  
CHANNEL_COUNT = 3  
CHANNEL_0 = CLEAN  
CHANNEL_1 = CRUNCH  
CHANNEL_2 = LEAD  
  
TUBES = 4  
HEATER_MAX = 1.35A  
BPLUS_MAX = 23mA  
  
AUDIO_INPUT_MAX = 4Vrms  
AUDIO_OUTPUT_MAX = 10Vrms

The host must be able to generate a usable interface for a compliant module it has never encountered before. Host firmware must not contain module-specific code such as TubeScreamer.setDrive().

# 33. Output Architecture

MODULE CHAIN  
\|  
+--------------+--------------+  
\| \| \|  
v v v  
RAW POWER SIM PATH FX SEND/RETURN  
AMP OUT \|  
v  
POWER-AMP MODEL  
\|  
CAB IR  
\|  
MIC / ROOM SIM  
\|  
+--------+--------+  
\| \|  
v v  
LINE OUT HEADPHONES

# 34. Raw Power-Amp Output

The raw output is a high-headroom analogue path for amplifier FX returns and external power amps. It receives no mandatory cabinet simulation and shall target 8–10 Vrms clean output with \<100 Ω output impedance.

Its level control must allow normal instrument/line operation while retaining the ability to drive compatible external guitar power-amplifier inputs hard.

# 35. Simulated and Headphone Outputs

- Simulation is modular and may include power-amp behaviour, cabinet IR, microphone and room models.

- Simulated line output should be stereo-capable.

- Headphones normally monitor the simulated path.

- Headphone volume is independent of raw power-amp output level.

- Reference headphone support target: 32–300 Ω.

# 36. MIDI, USB and Expression

- MIDI DIN and USB MIDI.

- Program Change and Control Change.

- Continuous parameters use 14-bit CC pairs or NRPN to preserve the 0-4095 logical resolution; 7-bit CC remains permitted for coarse control.

- MIDI mappings are host-side, preset-independent mapping objects (CC/NRPN to module/parameter, Program Change to preset), so off-the-shelf MIDI controllers and DAW automation act as primary control surfaces.

- A native remote surface is a Control Module attached to the host or RS-485 trunk; it receives parameter names, values and channel labels, which plain MIDI cannot carry.

- MIDI clock where relevant.

- At least two expression inputs recommended on reference host.

- USB for configuration, diagnostics, preset management, IR loading and firmware update.

- Future USB Audio is supported as an output-module capability.

# 37. DSP Architecture

DSP audio processing should normally be isolated from the main ESP32 host so that host UI/routing workload cannot disturb real-time audio. DSP modules choose their own processing technology.

| **Path**                         | **Preferred latency target** |
|----------------------------------|------------------------------|
| Individual real-time DSP effect  | \<1.5 ms                     |
| Complete amp/cab simulation path | \<3 ms                       |
| Raw analogue path                | Effectively zero DSP latency |

# 38. Mechanical Architecture

OMGP should use at least two mechanical families rather than forcing all circuitry into one universal 100×80 mm card.

- Compact FX cartridge format for high module density.

- Larger preamp cartridge format for valves, HV components and thermal spacing.

- Multi-width preamp modules allowed.

- Production cartridges should protect PCBs, aid keying and restrict finger access.

- Exact sizes and slot pitch remain a mechanical prototyping task.

# 39. Software Architecture

DESKTOP APPLICATION  
\|  
USB  
\|  
ESP32-S3 HOST  
\|  
RS-485 TRUNK  
\|  
+------------------+------------------+  
\| \| \|  
FX BACKPLANE PREAMP BACKPLANE DSP/SIM  
\| \|  
modules modules

- Host firmware: real-time system control, routing, presets, UI, MIDI, expression and power coordination.

- Module SDK: developer-facing Arduino-compatible interface.

- Desktop application: cross-platform routing, configuration, presets, IRs, updates and diagnostics.

- CLI: development, diagnostics and automated testing.

- DSP firmware: independent from host firmware.

- Open external API: enables alternative editors, Raspberry Pi controllers, DAW tools and test systems.

# 40. Desktop and CLI

The desktop application should target Windows, macOS and Linux. A lightweight implementation such as Rust/Tauri may be evaluated, but is not mandated by the protocol.

omgp modules  
omgp backplanes  
omgp inspect preamp-3  
omgp channel preamp-3 lead  
omgp set preamp-3 gain 72%  
omgp route fx-2 preamp-3 fx-7  
omgp power  
omgp diagnostics

# 41. Mock Hardware / Software-First Development

OMGP software development shall not wait for final PCBs. A virtual hardware environment should emulate modules, backplanes, power budgets, routing and faults using the same logical protocol as production hardware.

DESKTOP APP  
\|  
HOST CORE  
\|  
OMGP TRANSPORT API  
\|  
+-------------+--------------+  
\| \| \|  
Virtual FX Virtual Preamp Virtual DSP  
Backplane Backplane Module  
\| \|  
Green Overdrive/Fuzz, HiRise/HiGain-style preamps, etc.

The host core must not know whether messages are being carried by the real RS-485 trunk and I2C module buses, a native virtual bus or a development transport.

# 42. Transport Abstraction

class OMGPTransport {  
public:  
virtual bool send(const Message& msg) = 0;  
virtual bool receive(Message& msg) = 0;  
};  
  
TrunkTransport -\> RS-485 host trunk / real hardware  
I2CTransport -\> module-side SMBus peripheral  
VirtualTransport -\> native software tests  
UDPTransport -\> distributed development / optional HIL

Production embedded code may use an equivalent C/C++ interface appropriate to resource constraints; the abstraction is architectural, not a requirement to use C++ virtual methods literally.

# 43. Virtual Module Definitions

Virtual modules may be described using a human-readable format such as JSON or YAML. This allows new protocol and UI behaviour to be exercised without compiling firmware.

{  
"id": "preamp-1",  
"name": "HiRise 800",  
"type": "tube_preamp",  
"channels": \[  
{"name": "Clean"},  
{"name": "Crunch"},  
{"name": "Lead"}  
\],  
"power": {  
"heater_voltage": 6.3,  
"heater_max_a": 1.1,  
"bplus_voltage": 250,  
"bplus_max_ma": 18  
}  
}

# 44. Virtual Fault Injection

- B+ overcurrent

- Heater open/overcurrent

- Overtemperature

- Trunk timeout, I2C lockup or bridge failure

- Unexpected slot removal

- Invalid or incompatible descriptor

- Power-budget exceeded

- Channel-switch timeout

- Stuck/failing simulated relay

- Firmware/protocol mismatch

This allows safety and recovery logic to be exercised before high-voltage hardware exists.

# 45. Routing Simulation

Initial simulation models control-plane topology rather than audio waveform processing. The simulator validates whether requested graphs are legal and records the virtual switches/routes that would operate.

INPUT  
\|  
FX-03  
\|  
PREAMP-02 / Lead  
\|  
FX-11  
\|  
SIM  
\|  
OUTPUT

Optional real-time audio modelling can be added later, but is not a prerequisite for protocol or host development.

# 46. Hardware-in-the-Loop Development

Real ESP32-S3 Host  
\|  
USB / RS-485 adapter  
\|  
PC Simulation Environment  
\|  
Virtual Backplanes + Modules

The real host firmware should be able to control simulated modules before the final backplane is manufactured. Likewise, module firmware should support a simulation build where practical.

# 47. Automated Test Examples

TEST: Unknown module discovery  
  
1. Attach virtual module  
2. Receive descriptor  
3. Build channel/parameter model  
4. Change parameter  
5. Save preset  
6. Remove module  
7. Reinsert module  
8. Restore state  
  
Expected: PASS

TEST: Tube heater budget  
  
Backplane capacity = 5.0 A  
  
A = 1.3 A  
B = 1.2 A  
C = 1.4 A  
D = 1.5 A  
Total requested = 5.4 A  
  
Expected:  
D is not energised and host reports insufficient heater capacity.

A protocol compliance suite should eventually run against both virtual and real modules.

# 48. Recommended Repository Structure

omgp/  
\|-- spec/  
\| \|-- electrical/  
\| \|-- mechanical/  
\| \|-- protocol/  
\| \`-- safety/  
\|  
\|-- hardware/  
\| \|-- host/  
\| \|-- fx-backplane/  
\| \|-- preamp-backplane/  
\| \|-- routing/  
\| \|-- output/  
\| \|-- tube-psu/  
\| \|-- omgp-core/  
\| \`-- modules/  
\|  
\|-- software/  
\| \|-- host-core/  
\| \|-- esp32-host/  
\| \|-- arduino/  
\| \|-- module-sdk/  
\| \|-- desktop/  
\| \|-- cli/  
\| \|-- dsp/  
\| \`-- protocol/  
\|  
\|-- simulator/  
\| \|-- bus/  
\| \|-- backplane/  
\| \|-- modules/  
\| \|-- routing/  
\| \|-- power/  
\| \`-- faults/  
\|  
\|-- virtual-modules/  
\| \|-- green-overdrive.json  
\| \|-- dual-drive.json  
\| \|-- british-preamp.json  
\| \|-- four-tube-preamp.json  
\| \`-- delay.json  
\|  
\`-- tests/

# 49. Open Hardware Deliverables

- Full specification and version history

- KiCad schematics and PCB layouts

- Gerbers, BOM and pick-and-place files

- Mechanical CAD and cartridge templates

- Reference host and backplane designs

- OMGP Core design

- Tube PSU reference design and safety documentation

- Reference effect and preamp modules

- Arduino-compatible SDK

- Host firmware, CLI and desktop software

- Simulator and virtual module library

- Protocol/compliance test suite

# 50. Licensing Direction

Licensing remains to be formally selected. A plausible direction is CERN Open Hardware Licence for hardware, Apache-2.0 or MIT for firmware/software, and a Creative Commons licence for documentation. The project must explicitly decide whether third-party proprietary commercial modules may claim compatibility.

# 51. Engineering Validation Programme

| **Stage**                  | **Purpose**                                                                                    |
|----------------------------|------------------------------------------------------------------------------------------------|
| 1 — Audio characterisation | Measure guitars, pedals, tube preamps, FX returns and external power-amp inputs.               |
| 2 — Power budget           | Measure representative analogue, DSP and tube modules and derive realistic slot limits.        |
| 3 — Virtual protocol       | Develop discovery, channel, parameter, preset, power and routing logic entirely in simulation. |
| 4 — ESP32 HIL              | Run real host firmware against simulated backplanes/modules.                                   |
| 5 — FX backplane Rev A     | Validate trunk/I2C bridging, low-voltage rails, slot protection, audio routing and noise.      |
| 6 — Green Overdrive reference        | Validate digitally controlled analogue behaviour and presets.                                  |
| 7 — Routing prototype      | Validate ~10 Vrms headroom, switching, crosstalk and attenuation.                              |
| 8 — Tube PSU prototype     | Validate heater/B+, isolation, current protection, noise and discharge.                        |
| 9 — Tube preamp            | Measure real B+ current, heater load, channel switching and output levels.                     |
| 10 — Preamp expansion      | Validate several preamps/backplanes and distributed power management.                          |
| 11 — DSP/Sim               | Add cabinet/amp simulation, stereo output and headphone path.                                  |
| 12 — Community Alpha       | External advanced-DIY builders develop modules from the published spec alone.                  |

# 52. Decisions Not Yet Frozen

- Exact FX and preamp cartridge dimensions and slot pitch.

- Exact 56-contact connector manufacturer and mechanical keying.

- Expansion/global-audio connector families.

- HV tube connector family.

- Routing-matrix topology and switching ICs.

- Inter-slot attenuation implementation.

- Exact regulator/eFuse components and chassis-wide power supply.

- Tube PSU converter topology and validated B+ operating points.

- Detailed ground/chassis bonding implementation. The ground topology (AGND/GND/chassis strategy) must be selected before the module connector pinout and Backplane Rev A are frozen.

- Analogue rail voltage versus routing headroom: +/-15 V rails support approximately 8 Vrms through active stages; the ~10 Vrms routing target requires higher rails. One coherent decision is required before the connector pinout freezes.

- Trunk framing, CRC, poll scheduling and retry semantics; SMBus command set, discovery frames and descriptor serialization.

- Host-assisted module firmware update mechanism.

- Simulation DSP hardware and codec choices.

These items are intentionally unresolved until calculations and prototypes produce evidence. The logical architecture should guide testing rather than become frozen around untested component choices.

# 53. Immediate Software Milestone

- Define the initial OMGP message model and transport abstraction.

- Implement a native virtual control bus modelling both the RS-485 trunk and the per-backplane I2C segments.

- Implement virtual FX and preamp backplanes.

- Create virtual Green Overdrive, multi-channel drive, multi-channel tube preamp and delay modules.

- Implement power-budget management and fault injection.

- Model backplane bridge behaviour (poll scheduling, forwarding, slot supervision, fault confinement) in the virtual backplanes from the outset; the bridge firmware is the keystone of the system.

- Create a CLI that discovers modules, changes channels/parameters, saves presets and displays power state.

- Run the same host-core logic later on the ESP32-S3.

# 54. Immediate Hardware Milestone

- ESP32-S3 Host Dev board or development-board carrier.

- 2–4 slot FX Backplane Rev A.

- OMGP Core Rev A.

- Passive module breakout.

- Green Overdrive smart analogue reference module.

- High-headroom routing test board.

Tube hardware follows after the low-voltage architecture and software protocol are functioning, reducing the number of variables introduced during initial bring-up.

# 55. Definition of Success

OMGP v1 succeeds when an advanced DIY developer can download the specification and templates, create a module using their chosen analogue, tube or digital implementation, declare its channels/parameters/power/audio capabilities, insert it into an existing compatible host and have that host discover and control it without host-specific code.

**The interoperability test is the product.**

# Appendix A — Current Reference Baseline

| **Area**                            | **Current reference**                                                          |
|-------------------------------------|--------------------------------------------------------------------------------|
| Host MCU                            | ESP32-S3                                                                       |
| Module framework                    | Arduino-compatible SDK; MCU independent                                        |
| Control bus                         | Two-tier: I2C/SMBus module bus per backplane; RS-485 trunk, 1 Mbit/s reference |
| Protocol capacity                   | At least 64 addressable nodes/modules                                          |
| FX capacity                         | Expandable; reference 8–16 positions per host/backplane                        |
| Preamp capacity                     | 4 positions per preamp backplane; expandable using additional backplanes       |
| Module channels                     | Supported on all module types; dynamically declared                            |
| Analogue rails                      | ±15 V                                                                          |
| Legacy rail                         | +9 V                                                                           |
| Digital rails                       | +5 V and +3.3 V standby                                                        |
| Tube heater                         | 6.3 V DC                                                                       |
| Tube module maximum                 | 4 dual triodes / 8 triode sections within declared power envelope              |
| T4 reference                        | 2.0 A heater allocation / 35 mA B+ allocation                                  |
| Per-module B+ ceiling               | 40 mA provisional                                                              |
| B+ nominal                          | ~250 V; ~180–300 V architecture under validation                               |
| Reference preamp-backplane tube PSU | ~6.3 V / 5 A heater; ~250 V / 100 mA B+                                        |
| Raw output                          | 8–10 Vrms clean target, \<100 Ω                                                |
| Global audio fabric                 | Provision up to 8 buses; initial implementation may use 4                      |
| Normal module connector             | Logical 56-contact / 2×28 Alpha pinout                                         |
| Tube power connector                | Separate keyed HV connector, not yet frozen                                    |
| Mock development                    | Virtual modules/backplanes + native transport + ESP32 hardware-in-the-loop     |

# Appendix B — Safety Notice

Tube modules and tube power supplies can contain potentially lethal voltages and stored energy after power is removed. The values in this draft are architectural targets, not construction approval. High-voltage designs require appropriate clearance/creepage, insulation, enclosure, earthing, overcurrent protection, discharge, interlocking and competent review. Do not expose B+ through the ordinary module edge connector or assume software alone can provide safe shutdown.
