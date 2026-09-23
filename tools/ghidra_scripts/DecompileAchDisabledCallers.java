import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

public class DecompileAchDisabledCallers extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_achdisabled_callers.txt";
    static final long IMAGE_SHIFT = 0x100000L;
    // anywhere inside the ToString/name-lookup dispatcher function that contains
    // the "LoadAchievementsDisabled" case (case value 144)
    static final long PROBE_VMA = 0x3b1b420L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);

            Address probe = toAddr(PROBE_VMA + IMAGE_SHIFT);
            Function toStringFn = getFunctionContaining(probe);
            out.println("=== ToString/name-lookup dispatcher ===");
            if (toStringFn == null) {
                out.println("No function contains probe addr " + probe);
                out.close();
                return;
            }
            out.println("Function: " + toStringFn.getName() + " @ " + toStringFn.getEntryPoint()
                    + " size=" + toStringFn.getBody().getNumAddresses());

            // Find all callers of this function
            Set<Function> callers = new HashSet<>();
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(toStringFn.getEntryPoint());
            while (refs.hasNext()) {
                Reference r = refs.next();
                Function caller = getFunctionContaining(r.getFromAddress());
                if (caller != null) {
                    callers.add(caller);
                }
            }
            out.println("Total distinct callers: " + callers.size());
            out.println();

            int count = 0;
            for (Function caller : callers) {
                count++;
                out.println("=================================================================");
                out.println("=== Caller #" + count + ": " + caller.getName() + " @ " + caller.getEntryPoint()
                        + " size=" + caller.getBody().getNumAddresses() + " ===");
                DecompileResults res = decomp.decompileFunction(caller, 60, monitor);
                if (res != null && res.decompileCompleted()) {
                    out.println(res.getDecompiledFunction().getC());
                } else {
                    out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null")
                            + " (raw entry " + caller.getEntryPoint() + ")");
                }
                out.println();
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
