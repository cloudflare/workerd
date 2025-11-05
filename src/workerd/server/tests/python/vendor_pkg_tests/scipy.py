import numpy as np
from scipy import linalg


async def test():
    c = np.array([1, 3, 6, 10])
    r = np.array([1, -1, -2, -3])
    b = np.array([1, 2, 2, 5])
    res = linalg.solve_toeplitz((c, r), b)
    assert repr(res) == "array([ 1.66666667, -1.        , -2.66666667,  2.33333333])"
