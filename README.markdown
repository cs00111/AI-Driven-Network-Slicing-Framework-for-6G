# AI-Driven 6G Network Slicing Simulation

This project simulates a 5G/6G network slicing framework using NS-3, with a Python-based Deep Q-Network (DQN) model integrated via Pybind11 for dynamic bandwidth allocation. It models 40 User Equipments (UEs), 6 routers, and 1 server in a 50x50 m area, supporting eMBB, URLLC, and mMTC slices.

## Overview
- **Objective**: Optimize network slicing using a DQN model for SDN-controlled bandwidth allocation.
- **Components**: NS-3 v3.41, mmWave module, `DynamicTrafficApp`, Python DQN model, `life.csv` (metrics), `life.xml` (NetAnim visualization).
- **Metrics**: URLLC latency: ~0.2–0.8 ms, eMBB throughput: ~5–15 Mbps, packet loss: <1%.

## Prerequisites
- Ubuntu 20.04+, Python 3.8+
- NS-3 v3.41, Pybind11
- Python libraries: `requests`, `numpy`, `tensorflow` (or `torch` for PyTorch-based DQN)
- Tools: `g++`, `cmake`, NetAnim

Install:
```bash
sudo apt-get install g++ python3 python3-dev pybind11 cmake
pip3 install requests numpy tensorflow
```

## Setup
1. **Install NS-3**:
   ```bash
   git clone https://gitlab.com/nsnam/ns-3-dev.git
   cd ns-3-dev
   git checkout ns-3.41
   ./waf configure --enable-python-bindings
   ./waf
   ```

2. **Copy Files**:
   - Place `5gsim_enhanced_pybind.cc` and `rl_model.py` in `ns-3-dev/scratch/`.

3. **DQN Model**:
   - Update `MODEL_URL` in `rl_model.py` with your Google Drive link for the DQN model weights (e.g., `.h5` for TensorFlow).
   - Run:
     ```bash
     python3 rl_model.py
     ```

## Running
1. **Compile and Run**:
   ```bash
   cd ns-3-dev
   ./waf --run "scratch/5gsim_enhanced_pybind"
   ```

2. **Outputs**:
   - `life.csv`: Logs latency, throughput, queue size.
   - `life.xml`: NetAnim visualization (red=eMBB, green=URLLC, blue=mMTC).
   - Verify: URLLC ~0.2–0.8 ms, eMBB ~5–15 Mbps.

3. **Visualize**:
   ```bash
   netanim
   ```
   Open `life.xml` to view UE mobility and router congestion.

## Troubleshooting
- **Compilation**: Ensure Pybind11 and Python bindings are enabled (`./waf configure`).
- **Python**: Verify DQN model weights and libraries (`pip3 install tensorflow numpy requests`).
- **Metrics**: Check `life.csv` for anomalies (e.g., latency >1 ms).
- **NetAnim**: Confirm `life.xml` and UE colors.

## Notes
- Developed by K Venkata Bharadwaj (121CS0011) and Rama Krishna Aare (121CS0060) under Dr. R. Anil Kumar, IIITDM Kurnool.
- B.Tech end-review project (Apr 2025).