# MirkoNet

Bachelor thesis in resource limited blockchain implementation.

```text
mirkonet/
├── mirkonet.ino
├── assembler.h
├── blockchain.h
├── config.h
├── consensus.h
├── led.h
├── mvm.h
├── network.h
├── staking.h
├── txauth.h
├── types.h
└── wifi.h
```

- `mirkonet.ino`: Entry point, initialization, event loop, commands, and component integration
- `assembler.h`: Assmebler, turning instructions into mVM bytecode
- `blockchain.h`: Main blockchain logic, transactions, accounts, contracts, and checkpoints.
- `config.h`: Defines network limits, gas costs, timing, staking, and hardware settings.       
- `consensus.h`: Implements the consensus.
- `led.h`: Led colors and neopixel module.
- `mvm.h`: Virtual machine implementation.
- `network.h`: Handles peer discovery, gossip, synchronization, and contract transfer.
- `staking.h`: Implements staking, elections, rewards, and slashing.
- `txauth.h`: Creates and verifies transaction signatures.
- `types.h`: Contains shared data structures.
- `wifi.h`: Handles Wi-Fi configuration, the web dashboard, and the HTTP API.
