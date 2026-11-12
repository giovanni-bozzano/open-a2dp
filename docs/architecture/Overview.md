# System architecture

OpenA2dp replaces `BthA2dp.sys` per remote A2DP sink device, not the Bluetooth radio/core stack. Windows 11 x64 only; HFP/HSP and existing AVRCP handling remain with Microsoft. A2DP sink mode and LE Audio are outside scope.

## Components
| Component | Responsibility |
|---|---|
| `OpenA2dp.sys` (KMDF + ACX) | Audio endpoint, AVDTP, L2CAP BRBs, kernel SBC encoding and RTP transport |
| `OpenA2dpSvc.exe` | Windows service with a worker per sink; Media Foundation AAC-LC and the AOSP aptX/aptX HD encoders |
| `common/include/OpenA2dp/` | Kernel-safe ABI, rings, AVDTP/RTP and codec selection; no STL/exceptions |
| `codecs/` | Codec interfaces, SBC/AAC/aptX encoding and negotiation support |
| `OpenA2dpCtl.exe` | Diagnostic device/status/codec-preference CLI |
| `OpenA2dpConfig.exe` | Desktop settings window: sink list, live status and the codec/endpoint preference; see [configuration design](Codec-Configuration.md) |

## Data flow
```text
Windows audio engine -> ACX endpoint -> PCM ring
  SBC: kernel SbcFixed -> RTP -> L2CAP -> bthport -> Bluetooth transport/radio
  AAC: shared PCM ring -> service/MF -> LATM frame ring -> driver RTP -> same transport
  aptX: shared PCM ring -> service/AOSP -> frame ring -> driver RTP -> same transport
```

SBC stays integer-only in the kernel so mandatory audio does not depend on a service. AAC and the aptX codecs use user-mode encoders (Media Foundation and the vendored AOSP trees); the driver validates returned records and owns transport. See [encoder service boundary](Encoder-Service.md) for memory/trust boundaries.

The driver binds above Microsoft's `bthport`/`BthEnum`; `bthusb` or the vendor radio transport remains unchanged. One ACX render circuit represents each remote device. See [driver lifecycle](Driver-Lifecycle.md) for ownership and teardown constraints.

```mermaid
flowchart TD
    subgraph WIN["Windows sound system (user mode)"]
        APP["Apps playing sound"]
        AE["Windows audio engine"]
    end

    subgraph AACSVC["Encoder service (user mode)"]
        SVC["OpenA2dpSvc.exe<br/>compresses audio to AAC, aptX or aptX HD"]
    end

    subgraph TOOL["Diagnostic tool (user mode)"]
        CLI["OpenA2dpCtl.exe<br/>shows status, forces a codec"]
    end

    subgraph DRV["OpenA2dp driver (kernel mode)"]
        subgraph AUD["Audio endpoint"]
            ACX["Sound device seen by Windows<br/>PCM stereo, 16 kHz to 96 kHz<br/>16-bit, 24-in-32 for aptX HD and LDAC"]
            PCM[("PCM ring<br/>raw audio waiting to be encoded")]
        end

        subgraph NEG["Negotiation"]
            SES["AVDTP session<br/>agrees on a codec with the sink<br/>falls back from AAC to SBC"]
        end

        subgraph STR["Media streaming"]
            CLK["Media clock<br/>paces the stream"]
            SBC["SBC encoder<br/>built into the driver"]
            RTP["RTP packet builder"]
        end

        subgraph SVI["Service interface"]
            CTL["Control device<br/>accepts commands from the service<br/>and the diagnostic tool"]
            FR[("Frame ring<br/>encoded service frames")]
        end

        subgraph CHN["Bluetooth channels (L2CAP)"]
            SCH["Signaling channel<br/>carries negotiation messages"]
            MCH["Media channel<br/>carries the audio packets"]
        end
    end

    subgraph MS["Microsoft Bluetooth stack"]
        BTH["bthport<br/>Bluetooth core"]
        RAD["Radio driver"]
    end

    HP["Headphones or speaker"]

    APP --> AE --> ACX
    ACX -->|writes raw audio| PCM
    PCM -->|SBC route| SBC
    SBC --> RTP
    PCM -.->|AAC route: service reads audio| SVC
    SVC -.->|writes encoded frames| FR
    FR -.->|driver reads frames| RTP

    SES -->|starts the stream| CLK
    SES <-->|negotiation messages| SCH
    RTP --> MCH

    SCH --> BTH
    MCH --> BTH
    BTH --> RAD --> HP

    CLI -->|asks for status, forces a codec| CTL
    SVC -->|registers, maps rings, waits for ticks| CTL
    CTL -.->|AAC service ready: allow AAC| SES
    CLK -.->|each tick wakes the service| CTL

    linkStyle 5,6,7,17,18 stroke:#7F77DD,stroke-width:2px
```
