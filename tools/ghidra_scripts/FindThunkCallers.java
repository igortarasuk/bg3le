import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.app.cmd.function.CreateFunctionCmd;

import java.io.FileWriter;
import java.io.PrintWriter;

public class FindThunkCallers extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_thunk_callers.txt";
    static final long[] THUNKS = { 0x6232720L, 0x6232730L, 0x6232740L };
    static final String[] NAMES = { "HasMods", "HasMissingMods", "HasUnofficialMods" };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            ReferenceManager rm = currentProgram.getReferenceManager();
            for (int i = 0; i < THUNKS.length; i++) {
                Address thunkAddr = toAddr(THUNKS[i]);
                out.println("=== thunk for " + NAMES[i] + " @ " + thunkAddr + " ===");
                Function f = getFunctionAt(thunkAddr);
                if (f == null) {
                    CreateFunctionCmd cfc = new CreateFunctionCmd(thunkAddr);
                    cfc.applyTo(currentProgram, monitor);
                    f = getFunctionAt(thunkAddr);
                }
                out.println("function: " + (f != null ? f.getName() : "none"));
                int count = 0;
                for (Reference r : rm.getReferencesTo(thunkAddr)) {
                    Address fromAddr = r.getFromAddress();
                    Function caller = getFunctionContaining(fromAddr);
                    out.println("  ref from " + fromAddr + " type=" + r.getReferenceType() + " in " + (caller != null ? caller.getName() + "@" + caller.getEntryPoint() : "??? (no function)"));
                    count++;
                }
                out.println("total refs: " + count);
                out.println();
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
