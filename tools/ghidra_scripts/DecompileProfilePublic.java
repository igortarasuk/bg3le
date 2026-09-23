import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileProfilePublic extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_profilepublic_decomp.txt";
    static final long FUNC_ADDR = 0x05152560L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            Function f = getFunctionAt(toAddr(FUNC_ADDR));
            if (f == null) {
                out.println("no function found at " + toAddr(FUNC_ADDR));
                return;
            }
            out.println("Function: " + f.getName() + " @ " + f.getEntryPoint());
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            DecompileResults res = decomp.decompileFunction(f, 60, monitor);
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
            }

            out.println("--- callers of " + f.getName() + " (level 1) ---");
            ReferenceManager rm = currentProgram.getReferenceManager();
            java.util.List<Function> callers = new java.util.ArrayList<>();
            for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
                Address fromAddr = r.getFromAddress();
                Function caller = getFunctionContaining(fromAddr);
                out.println("  called from " + fromAddr + " in " + (caller != null ? caller.getName() + "@" + caller.getEntryPoint() : "???"));
                if (caller != null) callers.add(caller);
            }

            out.println();
            out.println("--- level 2 (callers of callers) ---");
            java.util.Set<String> seen = new java.util.HashSet<>();
            for (Function caller : callers) {
                for (Reference r : rm.getReferencesTo(caller.getEntryPoint())) {
                    Address fromAddr = r.getFromAddress();
                    Function caller2 = getFunctionContaining(fromAddr);
                    String s = "  " + caller.getName() + " <- called from " + fromAddr + " in " + (caller2 != null ? caller2.getName() + "@" + caller2.getEntryPoint() : "???");
                    if (seen.add(s)) out.println(s);
                }
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
