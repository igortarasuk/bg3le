import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileSGMVtableFuncs extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_sgm_vtable_funcs.txt";
    static final long[] FUNCS = {
        0x06636de0L, 0x06637d90L, 0x06637ea0L, 0x06637eb0L, 0x06637ec0L,
        0x06637ed0L, 0x05b281f0L, 0x05b28200L, 0x06637ee0L, 0x05b28210L,
        0x05b28220L, 0x05b40920L, 0x05b28230L, 0x05b28240L, 0x05b28250L,
        0x06638220L
    };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            for (long addr : FUNCS) {
                Address a = toAddr(addr);
                Function f = getFunctionAt(a);
                out.println("=== " + a + " (" + (f != null ? f.getName() : "no func") + ") ===");
                if (f == null) { out.println(); continue; }
                DecompileResults res = decomp.decompileFunction(f, 30, monitor);
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
