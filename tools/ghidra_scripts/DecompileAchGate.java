import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileAchGate extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_achgate_candidate.txt";
    static final long IMAGE_SHIFT = 0x100000L;

    static final Object[][] TARGETS = {
        {"FUN_03188d90 candidate gate, called from UnlockAchievement native handler", 0x3088d90L},
        {"caller helper at 241b390", 0x241b390L},
        {"ECS lookup A at 218e050", 0x218e050L},
        {"ECS lookup B at 218dc90", 0x218dc90L},
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
