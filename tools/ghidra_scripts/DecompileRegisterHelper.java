import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileRegisterHelper extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_register_helper.txt";
    static final long HELPER_ADDR = 0x04076160L;
    static final long DESC_HASUNOFFICIALMODS = 0x6232740L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            Function f = getFunctionAt(toAddr(HELPER_ADDR));
            if (f == null) {
                out.println("no function at helper addr; creating");
                ghidra.app.cmd.function.CreateFunctionCmd cfc = new ghidra.app.cmd.function.CreateFunctionCmd(toAddr(HELPER_ADDR));
                cfc.applyTo(currentProgram, monitor);
                f = getFunctionAt(toAddr(HELPER_ADDR));
            }
            if (f != null) {
                out.println("Function: " + f.getName() + " @ " + f.getEntryPoint());
                DecompInterface decomp = new DecompInterface();
                decomp.openProgram(currentProgram);
                DecompileResults res = decomp.decompileFunction(f, 60, monitor);
                if (res != null && res.decompileCompleted()) {
                    out.println(res.getDecompiledFunction().getC());
                } else {
                    out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
                }
            } else {
                out.println("STILL no function");
            }

            out.println();
            out.println("--- raw bytes at descriptor 0x" + Long.toHexString(DESC_HASUNOFFICIALMODS) + " (32 bytes) ---");
            Address descAddr = toAddr(DESC_HASUNOFFICIALMODS);
            StringBuilder hex = new StringBuilder();
            for (int i = 0; i < 32; i++) {
                byte b = getByte(descAddr.add(i));
                hex.append(String.format("%02x ", b & 0xff));
            }
            out.println(hex.toString());

            // Interpret as two 8-byte pointers (common: {getter_or_offset, vtable/typeinfo})
            long v0 = getLong(descAddr);
            long v1 = getLong(descAddr.add(8));
            out.println("as two int64: 0x" + Long.toHexString(v0) + ", 0x" + Long.toHexString(v1));

            // Check if v0 or v1 look like code addresses (i.e., point to a function)
            for (long candidate : new long[]{v0, v1}) {
                try {
                    Address ca = toAddr(candidate);
                    Function cf = getFunctionContaining(ca);
                    out.println("candidate 0x" + Long.toHexString(candidate) + " -> function: " + (cf != null ? cf.getName() + "@" + cf.getEntryPoint() : "none/not in a function"));
                } catch (Exception e) {
                    out.println("candidate 0x" + Long.toHexString(candidate) + " -> invalid address");
                }
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
