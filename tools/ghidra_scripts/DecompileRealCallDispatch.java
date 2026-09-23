import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileRealCallDispatch extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_real_call_dispatch.txt";
    static final long IMAGE_SHIFT = 0x100000L;

    // VMAs recovered live from the DIV wrap init dump in bg3le's own log
    // (TOsirisInitFunction slots, bias-subtracted): the actual generic
    // Osiris "call a native function by id" and "query" dispatchers
    // registered by the game itself (not inside libOsiris.so).
    static final Object[][] TARGETS = {
        {"g_real_call (DIV call dispatcher)", 0x2cd4500L},
        {"g_real_query (DIV query dispatcher)", 0x2cd3f40L},
        {"REAL UnlockAchievement native handler (outer)", 0x3767780L},
        {"REAL UnlockAchievement native handler (inner body)", 0x3767800L},
        {"helper at 0x3767580", 0x3767580L},
    };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);

            for (Object[] t : TARGETS) {
                String label = (String) t[0];
                long vma = (Long) t[1];
                long ghidraAddr = vma + IMAGE_SHIFT;
                Address addr = toAddr(ghidraAddr);

                out.println("=================================================================");
                out.println("=== " + label + " ===");
                out.println("VMA 0x" + Long.toHexString(vma) + " -> ghidra 0x" + Long.toHexString(ghidraAddr));

                Function f = getFunctionContaining(addr);
                if (f == null) {
                    out.println("No function contains this address");
                    continue;
                }
                out.println("Function: " + f.getName() + " @ " + f.getEntryPoint()
                        + "  size=" + f.getBody().getNumAddresses());

                DecompileResults res = decomp.decompileFunction(f, 120, monitor);
                if (res != null && res.decompileCompleted()) {
                    out.println(res.getDecompiledFunction().getC());
                } else {
                    out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
                }
                out.println();
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
