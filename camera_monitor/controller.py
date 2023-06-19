from multiprocessing import Queue

class Controller:
    def __init__(self, recording_queue: Queue, snapshot_queue: Queue):
        self.recording_queue = recording_queue
        self.snapshot_queue = snapshot_queue

    def recording_start(self, data):
        self.recording_queue.put(data)

    def recording_stop(self, data):
        self.recording_queue.put(data)

    def snapshot(self, timestamp: str):
        self.snapshot_queue.put(True)

    def __getstate__(self):
        return self.__dict__