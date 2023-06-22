import numpy as np

def normalize_depth(depth_buf: np.ndarray, amplitude_buf: np.ndarray, max_distance: float) -> np.ndarray:
    depth_buf = np.nan_to_num(depth_buf)

    depth_buf = (1 - (depth_buf/max_distance)) * max_distance

    amplitude_buf[amplitude_buf<=7] = np.nan
    amplitude_buf[amplitude_buf>7] = 1.0

    res = amplitude_buf * depth_buf

    return res