from multiprocessing import Queue

class Controller:
    def __init__(self, recording_queue: Queue, snapshot_queue: Queue):
        self.recording_queue = recording_queue
        self.snapshot_queue = snapshot_queue

    def recording_start(self):
        self.recording_queue.put(True)

    def recording_stop(self):
        self.recording_queue.put(False)

    def snapshot(self):
        self.snapshot_queue.put(True)

    def __getstate__(self):
        return self.__dict__