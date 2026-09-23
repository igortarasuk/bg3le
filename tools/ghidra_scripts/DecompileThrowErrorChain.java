import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Data;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileThrowErrorChain extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_throwerror_chain.txt";
    static final long IMAGE_SHIFT = 0x100000L;

    // NOTE: these are already Ghidra-space addresses (read directly off Ghidra's own
    // FUN_xxxxxxxx / DAT_xxxxxxxx names in a prior decompile) -- do NOT add IMAGE_SHIFT again.
    static final Object[][] TARGETS_GHIDRASPACE = {
        {"FUN_02d2cb20 (ReportLoadStatus, caller)", 0x2d2cb20L},
        {"FUN_03c1b340 (ToString-dispatcher / ThrowError-analog)", 0x3c1b340L},
        {"FUN_02d29950 (HasCustomMods wrapper, called with gEocServer+0x108)", 0x2d29950L},
        {"FUN_038675f0 (per-module check A)", 0x38675f0L},
        {"FUN_03867580 (per-module check B)", 0x3867580L},
        {"FUN_02d2a260 (cleanup after module iteration)", 0x2d2a260L},
    };

    void dumpGlobal(PrintWriter out, String label, long vma) {
        Address addr = toAddr(vma + IMAGE_SHIFT);
        Data d = getDataAt(addr);
        out.println(label + " @ 0x" + Long.toHexString(vma) + " (ghidra " + addr + ")");
        if (d != null) {
            out.println("  DataType: " + d.getDataType().getName() + "  Label: " + getSymbolAt(addr));
        } else {
            out.println("  No Data defined at that address");
        }
    }

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);

            for (Object[] t : TARGETS_GHIDRASPACE) {
                String label = (String) t[0];
                long ghidraAddr = (Long) t[1];
                Address addr = toAddr(ghidraAddr);

                out.println("=================================================================");
                out.println("=== " + label + " ===");
                out.println("ghidra addr 0x" + Long.toHexString(ghidraAddr) + " (raw VMA ~0x" + Long.toHexString(ghidraAddr - IMAGE_SHIFT) + ")");

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

            out.println("=================================================================");
            out.println("=== Global data probes ===");
            // DAT_07e19d20 and DAT_07c8c7f8 referenced (decompiler-relative) VMAs guessed from
            // the "DAT_xxxxxxxx" ghidra default names seen in the decompiled callers -- these
            // are ghidra-space addresses already (07e19d20 / 07c8c7f8), NOT raw VMAs needing shift.
            long[] rawGhidraGlobals = {0x07e19d20L, 0x07c8c7f8L, 0x07d8d680L, 0x07d8ebb0L};
            for (long g : rawGhidraGlobals) {
                Address addr = toAddr(g);
                Data d = getDataAt(addr);
                out.println("ghidra addr 0x" + Long.toHexString(g) + " (raw VMA ~0x" + Long.toHexString(g - IMAGE_SHIFT) + ")");
                if (d != null) {
                    out.println("  DataType: " + d.getDataType().getName() + "  Label: " + getSymbolAt(addr)
                            + "  Value: " + d.getDefaultValueRepresentation());
                } else {
                    out.println("  No Data defined there");
                }
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
