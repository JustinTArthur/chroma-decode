#!/usr/bin/env python3
"""Generate a tiny, structurally valid ONNX model for the NN load-path tests.

This is NOT a real chroma-decoding model. It is the smallest valid graph that
ONNX Runtime will load, so the public C ABI model loaders
(``chd_nn_model_load_from_file`` / ``chd_nn_model_load_from_memory``) and the
provider-attach path can be exercised in CI and on any contributor's machine
without shipping the real weights (which are large and separately licensed; see
``models/README.md``). The graph is a single Identity node — inference output is
meaningless. The tests only load and bind it; they never run inference on it.

Regenerate (committed output is ``tiny_identity.onnx`` alongside this script):

    python3 -m venv .venv
    . .venv/bin/activate
    pip install onnx
    python tests/fixtures/gen_tiny_onnx.py
"""
import os

import onnx
from onnx import TensorProto, helper

# Single Identity node: float input -> identical float output. The shape is
# arbitrary and small; nothing depends on it because no inference is run.
x = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, 8, 8])
y = helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 3, 8, 8])
node = helper.make_node("Identity", inputs=["input"], outputs=["output"])
graph = helper.make_graph([node], "tiny_identity", [x], [y])

model = helper.make_model(
    graph,
    producer_name="chromadec-tests",
    opset_imports=[helper.make_opsetid("", 13)],
)
# Pin a conservative IR version so older ONNX Runtime builds in CI accept it.
model.ir_version = 8
onnx.checker.check_model(model)

out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tiny_identity.onnx")
onnx.save(model, out_path)
print(f"wrote {out_path} ({os.path.getsize(out_path)} bytes)")
