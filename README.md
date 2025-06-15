# mp3codec~ - Real-time MP3 Compression External for Max/MSP

A Max/MSP external that applies authentic LAME MP3 compression artifacts to incoming audio streams in real-time. Perfect for creative sound design, lo-fi effects, and understanding MP3 compression characteristics.

## Features

- **Real-time MP3 encoding/decoding** using LAME library
- **Complete quality range** from 320 kbps (pristine) to 32 kbps (extreme compression)
- **Individual compression toggles** for precise control over artifacts
- **Authentic LAME processing** - encode and decode through actual MP3 format
- **Low latency** (~51ms total) suitable for real-time performance
- **Comprehensive latency reporting** for timing-critical applications

## Quick Start

```max
// Basic usage - extreme compression with all aggressive settings:
[mp3codec~ 9]

// High quality compression:
[mp3codec~ 0]

// Custom setup: quality 7, input boost, output cut, start bypassed:
[mp3codec~ 7 2.0 0.5 1]
```

## Quality Levels

| Quality | Bitrate | Use Case |
|---------|---------|----------|
| 0 | 320 kbps | Pristine (subtle artifacts) |
| 1 | 256 kbps | High quality |
| 2 | 192 kbps | CD quality |
| 3 | 160 kbps | Good quality |
| 4 | 128 kbps | Standard quality |
| 5 | 112 kbps | Default |
| 6 | 96 kbps | Noticeable compression |
| 7 | 64 kbps | Low quality |
| 8 | 40 kbps | Very low quality |
| 9 | 32 kbps | Extreme compression |

## Individual Compression Controls

All aggressive settings are **enabled by default**. Disable them selectively:

```max
[lowpass 0<      // Disable 4kHz low-pass filter
[highpass 0<     // Disable 100Hz high-pass filter  
[msstereo 0<     // Disable forced mid/side stereo
[athonly 0<      // Disable ATH-only psychoacoustic model
[experimental 0< // Disable experimental compression modes
[emphasis 0<     // Disable pre-emphasis
```

## Messages

| Message | Arguments | Description |
|---------|-----------|-------------|
| `quality` | 0-9 | Change compression quality level |
| `bypass` | 0/1 | Enable/disable processing bypass |
| `reset` | - | Reset encoder/decoder state |
| `latency` | - | Report detailed latency analysis |
| `lowpass` | 0/1 | Toggle aggressive low-pass filtering |
| `highpass` | 0/1 | Toggle high-pass filtering |
| `msstereo` | 0/1 | Toggle forced mid/side stereo |
| `athonly` | 0/1 | Toggle ATH-only psychoacoustic model |
| `experimental` | 0/1 | Toggle experimental compression |
| `emphasis` | 0/1 | Toggle pre-emphasis |

## Constructor Arguments

```max
[mp3codec~ quality input_gain output_gain bypass]
```

- **quality** (0-9): MP3 quality level (default: 5)
- **input_gain** (0.0-4.0): Input gain multiplier (default: 1.0)
- **output_gain** (0.0-4.0): Output gain multiplier (default: 1.0)
- **bypass** (0/1): Start in bypass mode (default: 0)

## Outlets

- **Left/Right Audio**: Processed stereo audio output
- **Analysis**: Latency data and analysis information
- **Status**: Status messages and notifications

## Technical Details

### Latency Characteristics
- **LAME Encoder Delay**: ~576 samples (13.1ms @ 44.1kHz)
- **LAME Decoder Delay**: ~528 samples (12.0ms @ 44.1kHz)  
- **Buffer Latency**: 1152 samples (26.1ms @ 44.1kHz)
- **Total Latency**: ~2256 samples (51.2ms @ 44.1kHz)

### Audio Processing Chain
```
Audio Input → Accumulator (1152 samples) → LAME Encoder → 
MP3 Data → LAME hip Decoder → Ring Buffer → Audio Output
```

### Aggressive Compression Effects (Quality 9 + All Toggles)
- **Bandwidth**: Limited to 100Hz - 4kHz (telephone quality)
- **Stereo imaging**: Collapsed to near-mono via mid/side encoding
- **Quantization noise**: Heavy artifacts from ATH-only model
- **Metallic coloration**: Pre-emphasis distortion
- **Experimental artifacts**: LAME's most aggressive processing modes

## Building

### Prerequisites
- Max SDK (included in this package)
- LAME library (libmp3lame.a)
- CMake 3.19+
- macOS with Xcode command line tools

### Build Commands
```bash
cd source/audio/mp3codec~
mkdir build && cd build
cmake -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" ..
cmake --build .
```

The external will be built to: `externals/mp3codec~.mxo`

## Use Cases

### Creative Sound Design
- **Lo-fi textures**: Quality 7-9 for vintage digital artifacts
- **Telephone effects**: Enable only lowpass + highpass filters
- **Stereo width manipulation**: Use msstereo toggle
- **Bandwidth limiting**: Controlled frequency reduction

### Educational/Analysis
- **Compression artifacts study**: Compare quality levels
- **Psychoacoustic model demonstration**: athonly toggle effects
- **Codec latency measurement**: Built-in latency reporting
- **MP3 algorithm understanding**: Individual setting isolation

### Live Performance
- **Real-time lo-fi processing**: Low enough latency for live use
- **Dynamic quality changes**: Real-time quality level switching
- **Effect automation**: Message-based control integration

## Known Limitations

- **32 kbps minimum**: LAME enforces minimum 32 kbps CBR bitrate
- **Stereo only**: Currently configured for 2-channel processing
- **Fixed sample rates**: Optimized for 44.1kHz (will work at other rates)

## Troubleshooting

### External Won't Load
- Ensure LAME library is properly linked
- Check external is properly code-signed on macOS
- Verify Max SDK version compatibility

### Quality Changes Cause Crashes
- Use individual toggles instead of rapid quality changes
- Allow initialization to complete between changes
- Check Max console for error messages

### High CPU Usage
- Lower quality levels use more CPU (complex psychoacoustic models)
- Individual toggles may be more efficient than combined settings
- Consider bypass mode for CPU-intensive patches

## Credits

- **LAME MP3 Encoder**: https://lame.sourceforge.io/
- **Max SDK**: Cycling '74
- **Development**: Claude Code assistance

## License

This external uses the LAME MP3 encoder library. Please comply with LAME's LGPL license requirements for distribution.
