import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompilePerSaveCallback extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_persave_callback.txt";
    static final long FUNC_ADDR = 0x0662e840L;

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
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
