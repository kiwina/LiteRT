#!/usr/bin/env python3
"""Fix LiteRT partition serialization bug: broken operator code table.

Copies the complete operator_codes table from partition_0 (which has all
original codes from the model) and rebuilds the partition tflite with
proper builtin_options defaults.
"""
import sys

OP_DEFAULTS = {
    0: (9, 2), 2: (4, 1), 3: (1, 7), 4: (2, 7), 6: (28, 0),
    9: (40, 5), 18: (12, 1), 22: (5, 1), 25: (8, 1), 28: (32, 0),
    34: (6, 1), 39: (0, 0), 41: (0, 0), 43: (29, 0), 46: (30, 0),
    47: (19, 2), 49: (16, 1), 53: (0, 0), 55: (21, 0), 57: (22, 0),
    64: (35, 1), 65: (23, 0), 66: (27, 0), 69: (13, 0), 72: (24, 1),
    74: (0, 0), 76: (15, 0), 80: (20, 0), 83: (18, 1), 84: (0, 0),
    85: (7, 1), 86: (10, 9), 87: (11, 1), 88: (14, 1), 90: (0, 0),
    91: (33, 1), 93: (34, 0), 94: (31, 0), 98: (26, 2), 106: (25, 4),
    108: (0, 0), 126: (0, 0), 123: (0, 0), 32: (0, 0),
}

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "partition.tflite"
    from tflite.Model import Model
    import flatbuffers, os, glob

    buf = open(path, "rb").read()
    model = Model.GetRootAsModel(buf, 0)

    max_idx = 0
    for si in range(model.SubgraphsLength()):
        sg = model.Subgraphs(si)
        for oi in range(sg.OperatorsLength()):
            idx = sg.Operators(oi).OpcodeIndex()
            if idx > max_idx:
                max_idx = idx

    if max_idx < model.OperatorCodesLength():
        print("OPCODE_TABLE_OK")
        return 0

    # Find partition_0 which has the full operator code table
    needed_codes = {}
    oc_source = None
    for p in ["/tmp/litert_nbg_partition_0/partition.tflite",
              "../litert_nbg_partition_0/partition.tflite"]:
        if os.path.exists(p):
            oc_source = p
            break

    if oc_source:
        print("FIXING: copying operator codes from %s (%d entries, need %d)" % (
            oc_source, model.OperatorCodesLength(), max_idx + 1))
        src_buf = open(oc_source, "rb").read()
        src_model = Model.GetRootAsModel(src_buf, 0)
        for i in range(src_model.OperatorCodesLength()):
            oc = src_model.OperatorCodes(i)
            needed_codes[i] = (oc.BuiltinCode(), oc.CustomCode())
    else:
        print("FIXING: inferring operator codes (no partition_0 found)")
        OPT_TO_OP = {
            0: 0, 1: 3, 2: 4, 3: 1, 4: 2, 5: 22, 6: 34, 7: 85, 8: 25,
            9: 0, 10: 86, 11: 87, 12: 18, 13: 69, 14: 88, 15: 76,
            16: 49, 17: 90, 18: 83, 19: 47, 20: 80, 21: 55, 22: 57,
            23: 65, 24: 72, 25: 106, 26: 98, 27: 84, 28: 6, 29: 43,
            30: 46, 31: 94, 32: 28, 33: 91, 34: 93, 35: 64,
        }
        for si in range(model.SubgraphsLength()):
            sg = model.Subgraphs(si)
            for oi in range(sg.OperatorsLength()):
                op = sg.Operators(oi)
                idx = op.OpcodeIndex()
                if idx in needed_codes:
                    continue
                if op.CustomOptionsLength() > 0:
                    needed_codes[idx] = (32, b"DISPATCH_OP")
                elif idx < model.OperatorCodesLength():
                    oc = model.OperatorCodes(idx)
                    needed_codes[idx] = (oc.BuiltinCode(), oc.CustomCode())
                else:
                    needed_codes[idx] = (OPT_TO_OP.get(op.BuiltinOptionsType(), 0), None)

    b = flatbuffers.Builder(len(buf) + 65536)

    # Operator codes
    oc_offsets = []
    for idx in range(max_idx + 1):
        code, custom = needed_codes.get(idx, (0, None))
        custom_str_off = b.CreateString(custom) if custom else 0
        b.StartObject(4)
        b.PrependByteSlot(0, code if code < 127 else 127, 0)
        if custom_str_off:
            b.PrependUOffsetTRelativeSlot(1, custom_str_off, 0)
        b.PrependInt32Slot(2, 0, 0)
        b.PrependInt32Slot(3, code, 0)
        oc_offsets.append(b.EndObject())
    b.StartVector(4, len(oc_offsets), 4)
    for off in reversed(oc_offsets):
        b.PrependUOffsetTRelative(off)
    op_codes_vec = b.EndVector()

    # Buffers
    buf_offsets = []
    for bi in range(model.BuffersLength()):
        sb = model.Buffers(bi)
        data = sb.DataAsNumpy() if sb.DataLength() > 0 else None
        dv = 0
        if data is not None:
            b.StartVector(1, len(data), 1)
            for bv in reversed(data):
                b.PrependByte(int(bv))
            dv = b.EndVector()
        b.StartObject(3)
        if dv:
            b.PrependUOffsetTRelativeSlot(0, dv, 0)
        buf_offsets.append(b.EndObject())
    b.StartVector(4, len(buf_offsets), 4)
    for off in reversed(buf_offsets):
        b.PrependUOffsetTRelative(off)
    buffers_vec = b.EndVector()

    # Subgraphs
    sg_offsets = []
    for si in range(model.SubgraphsLength()):
        sg = model.Subgraphs(si)
        tensor_offsets = []
        for ti in range(sg.TensorsLength()):
            t = sg.Tensors(ti)
            sl = t.ShapeLength()
            b.StartVector(4, sl, 4)
            for di in reversed(range(sl)):
                b.PrependInt32(t.Shape(di))
            sv = b.EndVector()
            no = b.CreateString(t.Name().decode() if t.Name() else "")
            b.StartObject(11)
            b.PrependUOffsetTRelativeSlot(0, sv, 0)
            b.PrependByteSlot(1, t.Type(), 0)
            b.PrependUint32Slot(2, t.Buffer(), 0)
            b.PrependUOffsetTRelativeSlot(3, no, 0)
            tensor_offsets.append(b.EndObject())
        b.StartVector(4, len(tensor_offsets), 4)
        for off in reversed(tensor_offsets):
            b.PrependUOffsetTRelative(off)
        tensors_vec = b.EndVector()

        op_offsets = []
        for oi in range(sg.OperatorsLength()):
            op = sg.Operators(oi)
            il = op.InputsLength()
            b.StartVector(4, il, 4)
            for ii in reversed(range(il)):
                b.PrependInt32(op.Inputs(ii))
            iv = b.EndVector()
            ol = op.OutputsLength()
            b.StartVector(4, ol, 4)
            for ii in reversed(range(ol)):
                b.PrependInt32(op.Outputs(ii))
            ov = b.EndVector()
            co = 0
            if op.CustomOptionsLength() > 0:
                cd = op.CustomOptionsAsNumpy()
                b.StartVector(1, len(cd), 1)
                for bv in reversed(cd):
                    b.PrependByte(int(bv))
                co = b.EndVector()
            code = needed_codes.get(op.OpcodeIndex(), (0, None))[0]
            ot, nf = OP_DEFAULTS.get(code, (0, 0))
            bo = 0
            if ot > 0:
                b.StartObject(nf)
                bo = b.EndObject()
            b.StartObject(7)
            b.PrependUint32Slot(0, op.OpcodeIndex(), 0)
            b.PrependUOffsetTRelativeSlot(1, iv, 0)
            b.PrependUOffsetTRelativeSlot(2, ov, 0)
            b.PrependByteSlot(3, ot, 0)
            if bo:
                b.PrependUOffsetTRelativeSlot(4, bo, 0)
            if co:
                b.PrependUOffsetTRelativeSlot(5, co, 0)
                b.PrependByteSlot(6, op.CustomOptionsFormat(), 0)
            op_offsets.append(b.EndObject())
        b.StartVector(4, len(op_offsets), 4)
        for off in reversed(op_offsets):
            b.PrependUOffsetTRelative(off)
        operators_vec = b.EndVector()

        sil = sg.InputsLength()
        b.StartVector(4, sil, 4)
        for ii in reversed(range(sil)):
            b.PrependInt32(sg.Inputs(ii))
        siv = b.EndVector()
        sol = sg.OutputsLength()
        b.StartVector(4, sol, 4)
        for ii in reversed(range(sol)):
            b.PrependInt32(sg.Outputs(ii))
        sov = b.EndVector()
        sname = b.CreateString(sg.Name().decode() if sg.Name() else "main")
        b.StartObject(5)
        b.PrependUOffsetTRelativeSlot(0, tensors_vec, 0)
        b.PrependUOffsetTRelativeSlot(1, siv, 0)
        b.PrependUOffsetTRelativeSlot(2, sov, 0)
        b.PrependUOffsetTRelativeSlot(3, operators_vec, 0)
        b.PrependUOffsetTRelativeSlot(4, sname, 0)
        sg_offsets.append(b.EndObject())
    b.StartVector(4, len(sg_offsets), 4)
    for off in reversed(sg_offsets):
        b.PrependUOffsetTRelative(off)
    subgraphs_vec = b.EndVector()

    do = b.CreateString(model.Description().decode() if model.Description() else "")
    b.StartObject(5)
    b.PrependUint32Slot(0, model.Version(), 0)
    b.PrependUOffsetTRelativeSlot(1, op_codes_vec, 0)
    b.PrependUOffsetTRelativeSlot(2, subgraphs_vec, 0)
    b.PrependUOffsetTRelativeSlot(3, do, 0)
    b.PrependUOffsetTRelativeSlot(4, buffers_vec, 0)
    mo = b.EndObject()
    b.Finish(mo)
    nb = bytes(b.Output())
    open(path, "wb").write(nb)
    print("FIXED: %d opcodes, %d bytes" % (max_idx + 1, len(nb)))
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("FIX_ERROR: %s" % str(e))
        import traceback
        traceback.print_exc()
        sys.exit(1)
