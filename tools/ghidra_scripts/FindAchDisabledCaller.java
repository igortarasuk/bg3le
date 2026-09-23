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

public class FindAchDisabledCaller extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_achdisabled_root_callers.txt";
    // already ghidra-space (FUN_02d2cb20 from prior decompile)
    static final long REPORTER_ADDR = 0x02d2cb20L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);

            Address reporterAddr = toAddr(REPORTER_ADDR);
            Function reporter = getFunctionAt(reporterAddr);
            out.println("Reporter function: " + (reporter != null ? reporter.getName() : "NULL") + " @ " + reporterAddr);

            Set<Function> callers = new HashSet<>();
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(reporterAddr);
            while (refs.hasNext()) {
                Reference r = refs.next();
                Function caller = getFunctionContaining(r.getFromAddress());
                if (caller != null) {
                    callers.add(caller);
                }
            }
            out.println("Total distinct callers of the reporter: " + callers.size());
            out.println();

            int count = 0;
            for (Function caller : callers) {
                count++;
                out.println("=================================================================");
                out.println("=== Caller #" + count + ": " + caller.getName() + " @ " + caller.getEntryPoint()
                        + " size=" + caller.getBody().getNumAddresses() + " ===");
                DecompileResults res = decomp.decompileFunction(caller, 60, monitor);
                if (res != null && res.decompileCompleted()) {
                    String code = res.getDecompiledFunction().getC();
                    out.println(code);
                    if (code.contains("0x90") || code.contains(",144") || code.contains(", 144")) {
                        out.println(">>> CONTAINS 0x90/144 LITERAL <<<");
                    }
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
