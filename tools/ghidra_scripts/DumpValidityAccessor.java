import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpValidityAccessor extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_validity_accessor.txt";
    static final long VTABLE_ADDR = 0x07a69288L;
    static final long EXTRA_ADDR = 0x062326f0L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            out.println("=== vtable at " + toAddr(VTABLE_ADDR) + " (block=" + currentProgram.getMemory().getBlock(toAddr(VTABLE_ADDR)) + ") ===");
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            for (int i = 0; i < 8; i++) {
                Address slot = toAddr(VTABLE_ADDR).add((long) i * 8);
                long val = getLong(slot);
                Function f = getFunctionAt(toAddr(val));
                out.println(slot + ": 0x" + Long.toHexString(val) + (f != null ? " -> " + f.getName() : " (not a function)"));
                if (f != null) {
                    DecompileResults res = decomp.decompileFunction(f, 30, monitor);
                    if (res != null && res.decompileCompleted()) {
                        out.println(res.getDecompiledFunction().getC());
                    }
                }
            }

            out.println();
            out.println("=== extra data at " + toAddr(EXTRA_ADDR) + " (block=" + currentProgram.getMemory().getBlock(toAddr(EXTRA_ADDR)) + ") ===");
            for (int i = 0; i < 4; i++) {
                Address slot = toAddr(EXTRA_ADDR).add((long) i * 8);
                long val = getLong(slot);
                Function f = getFunctionAt(toAddr(val));
                out.println(slot + ": 0x" + Long.toHexString(val) + (f != null ? " -> " + f.getName() : ""));
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
