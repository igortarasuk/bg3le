import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileKeyFunctions extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_key_functions_decompiled.txt";
    static final long IMAGE_SHIFT = 0x100000L;

    // (label, VMA from objdump/readelf/nm)
    static final Object[][] TARGETS = {
        {"per_module_processor (LoadSavegame/LoadModuleAndLevel shared body)", 0x41e6ee0L},
        {"GUID_list_equality_primitive", 0x4015060L},
        {"caller1 enclosing function (state==0xf check + call)", 0x3c41300L},
        {"caller2 enclosing function (DoState)", 0x4016400L},
        {"LoadModule-specific handler", 0x6efd8b0L},
        {"ModListCache-related function", 0x7538ef0L},
        {"log-once helper (6efd3f0)", 0x6efd3f0L},
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
                    out.println("No function contains this address (still undefined even after full analysis)");
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
