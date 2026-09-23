import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

public class GetMergeCallers extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_merge_callers_full.txt";
    static final long FUNC_ADDR = 0x02f06590L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            Function f = getFunctionAt(toAddr(FUNC_ADDR));
            if (f == null) {
                out.println("no function at " + toAddr(FUNC_ADDR));
                return;
            }
            ReferenceManager rm = currentProgram.getReferenceManager();
            Set<String> callers = new HashSet<>();
            for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
                Address fromAddr = r.getFromAddress();
                Function caller = getFunctionContaining(fromAddr);
                if (caller != null) callers.add(caller.getName() + "@" + caller.getEntryPoint());
            }
            for (String s : callers) out.println(s);
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH + " with callers");
    }
}
