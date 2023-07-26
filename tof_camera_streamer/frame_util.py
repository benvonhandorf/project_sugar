import numpy as np

def filter_depth_with_amplitude(depth_buf: np.ndarray, amplitude_buf: np.ndarray, amplitude_cutoff_value=4) -> np.ndarray:
    amplitude_buf = amplitude_buf.copy()

    amplitude_buf[amplitude_buf <= amplitude_cutoff_value] = np.nan #Eliminate these depth values
    amplitude_buf[amplitude_buf > amplitude_cutoff_value] = 1.0 # 

    res = amplitude_buf * depth_buf

    return res

def convert_depth_to_meters(depth_buf: np.ndarray, max_distance: float):
    depth_buf = depth_buf * max_distance

    return depth_buf