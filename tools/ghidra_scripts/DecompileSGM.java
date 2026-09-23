import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileSGM extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_sgm_decomp.txt";
    static final long FUNC_ADDR = 0x06637ec0L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            Function f = getFunctionAt(toAddr(FUNC_ADDR));
            out.println("Function: " + (f != null ? f.getName() + " @ " + f.getEntryPoint() : "none"));
            if (f != null) {
                DecompInterface decomp = new DecompInterface();
                decomp.openProgram(currentProgram);
                DecompileResults res = decomp.decompileFunction(f, 60, monitor);
                if (res != null && res.decompileCompleted()) {
                    out.println(res.getDecompiledFunction().getC());
                } else {
                    out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
                }
                out.println("--- callers ---");
                ReferenceManager rm = currentProgram.getReferenceManager();
                for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
                    Address fromAddr = r.getFromAddress();
                    Function caller = getFunctionContaining(fromAddr);
                    out.println("  called from " + fromAddr + " in " + (caller != null ? caller.getName() + "@" + caller.getEntryPoint() : "???"));
                }
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
