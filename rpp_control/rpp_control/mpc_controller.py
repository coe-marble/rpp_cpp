from __future__ import annotations

import numpy as np

from rpp_plugin_types.rpp_common import MotionController2D as Controller
from rpp_common import ParameterDescription as Parameter


class MPCController(Controller):
    """ Model Predictive Controller plugin for rpp::Controller. This is a placeholder implementation and should be extended with actual MPC logic."""

    param_description = [
        Parameter(name="L", default_value=1.0, description="Proportional gain"),
    ]

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._integral = None
        self._previous_error = None

    def name(self) -> str:
        return "MPCController"

    def on_configure(self):
        self._integral = None
        self._previous_error = None
        return True

    def on_step(self, y_ref, y, dt, *args):
        y_ref_vec = np.atleast_1d(np.asarray(y_ref, dtype=float))
        y_vec = np.atleast_1d(np.asarray(y, dtype=float))
        error = y_ref_vec - y_vec

        kp = float(self.params.get("kp", 1.0)) if isinstance(self.params, dict) else 1.0
        ki = float(self.params.get("ki", 0.0)) if isinstance(self.params, dict) else 0.0
        kd = float(self.params.get("kd", 0.0)) if isinstance(self.params, dict) else 0.0

        if self._integral is None:
            self._integral = np.zeros_like(error)
        self._integral = self._integral + error * float(dt)

        if self._previous_error is None:
            derivative = np.zeros_like(error)
        else:
            derivative = (error - self._previous_error) / max(float(dt), 1e-9)

        self._previous_error = error
        return kp * error + ki * self._integral + kd * derivative

    def on_reset(self):
        self._integral = None
        self._previous_error = None
        return True
