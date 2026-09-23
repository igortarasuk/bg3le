import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.Set;

public class DecompileUnlockPath extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_unlock_path.txt";
    // Ghidra-space addresses (raw VMA + 0x100000)
    static final Object[][] DECOMP = {
        {"FUN_0398e8e0 (callee of gate-open path in UnlockAchievement handler)", 0x398e8e0L},
        {"FUN_03867890 (callee of inner body)", 0x3867890L},
        {"FUN_038675f0 (per-module predicate)", 0x38675f0L},
    };
    static final Object[][] XREFS = {
        {"FUN_03867580 (HasCustomMods-equivalent)", 0x3867580L},
        {"FUN_038675f0 (per-module predicate)", 0x38675f0L},
        {"FUN_0398e8e0", 0x398e8e0L},
    };

    void dumpFn(PrintWriter out, DecompInterface decomp, Function f) throws Exception {
        out.println("Function: " + f.getName() + " @ " + f.getEntryPoint()
                + " size=" + f.getBody().getNumAddresses()
                + " noreturn=" + f.hasNoReturn()
                + " callingConv=" + f.getCallingConventionName());
        Set<Function> callees = f.getCalledFunctions(monitor);
        out.print("Callees:");
        for (Function c : callees) out.print(" " + c.getName() + "@" + c.getEntryPoint() + (c.hasNoReturn() ? "(noreturn)" : ""));
        out.println();
        DecompileResults res = decomp.decompileFunction(f, 180, monitor);
        if (res != null && res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
        else out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
        out.println();
    }

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            for (Object[] t : DECOMP) {
                out.println("=================================================================");
                out.println("=== " + t[0] + " ===");
                Function f = getFunctionContaining(toAddr((Long) t[1]));
                if (f == null) { out.println("no function"); continue; }
                dumpFn(out, decomp, f);
            }
            for (Object[] t : XREFS) {
                out.println("=================================================================");
                out.println("=== XREFS to " + t[0] + " ===");
                Address a = toAddr((Long) t[1]);
                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
                int n = 0;
                while (it.hasNext()) {
                    Reference r = it.next();
                    Function caller = getFunctionContaining(r.getFromAddress());
                    out.println("  from " + r.getFromAddress() + " type=" + r.getReferenceType()
                            + " in " + (caller != null ? caller.getName() + "@" + caller.getEntryPoint() : "<none>"));
                    n++;
                }
                out.println("  total=" + n);
            }
        } finally { out.close(); }
        println("Done, wrote " + OUT_PATH);
    }
}
