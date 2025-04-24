import requests
import pickle
import numpy as np
import os

# Download pre-trained Q-table model
MODEL_URL = "https://drive.google.com/uc?export=download&id=1XXXXXXXXXXXXXXXXXXXX"  # Replace with actual Google Drive ID
MODEL_PATH = "q_table.pkl"

def download_model():
    if not os.path.exists(MODEL_PATH):
        print("Downloading RL model...")
        response = requests.get(MODEL_URL)
        with open(MODEL_PATH, "wb") as f:
            f.write(response.content)
        print("Model downloaded successfully")

def load_model():
    with open(MODEL_PATH, "rb") as f:
        q_table = pickle.load(f)
    return q_table

# Initialize model
download_model()
q_table = load_model()

def choose_action(state, traffic_stats, server_cpu, server_memory):
    """
    Choose bandwidth allocation based on state and Q-table.
    Args:
        state (int): Network state (0: eMBB, 1: URLLC, 2: mMTC, 3: Overloaded)
        traffic_stats (list): Throughput for [eMBB, URLLC, mMTC]
        server_cpu (float): Server CPU usage
        server_memory (float): Server memory usage
    Returns:
        list: Bandwidth splits [eMBB, URLLC, mMTC]
    """
    # Normalize inputs for Q-table lookup
    traffic_sum = sum(traffic_stats) + 1e-6
    norm_traffic = [t / traffic_sum for t in traffic_stats]
    cpu_load = min(server_cpu / 300.0, 1.0)
    memory_load = min(server_memory / 300.0, 1.0)
    
    # Create state key (simplified for Q-table)
    state_key = (state, int(norm_traffic[0] * 10), int(norm_traffic[1] * 10), int(cpu_load * 5))
    
    # Get action from Q-table
    if state_key in q_table:
        action_idx = np.argmax(q_table[state_key])
    else:
        action_idx = 3  # Default: balanced allocation
    
    # Map action to bandwidth splits
    bandwidth_splits = [
        [0.6, 0.2, 0.2],  # Action 0: eMBB dominant
        [0.2, 0.6, 0.2],  # Action 1: URLLC dominant
        [0.2, 0.2, 0.6],  # Action 2: mMTC dominant
        [0.33, 0.33, 0.34] # Action 3: Balanced
    ]
    
    return bandwidth_splits[action_idx]

if __name__ == "__main__":
    # Test the model
    test_state = 0
    test_traffic = [50.0, 20.0, 10.0]
    test_cpu = 200.0
    test_memory = 150.0
    action = choose_action(test_state, test_traffic, test_cpu, test_memory)
    print(f"Test action: {action}")