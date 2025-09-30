import numpy as np
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    def test(self):
        res = np.arange(12).reshape((3, -1))[::-2, ::-2]
        assert str(res) == "[[11  9]\n [ 3  1]]"
