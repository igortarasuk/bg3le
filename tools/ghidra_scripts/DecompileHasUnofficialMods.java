import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class DecompileHasUnofficialMods extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_hasunofficialmods_decomp.txt";
    static final long[] REF_ADDRS = { 0x04075fa9L, 0x07a16998L };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);

            for (long refLong : REF_ADDRS) {
                Address refAddr = toAddr(refLong);
                out.println("=== ref address " + refAddr + " ===");
                Function f = getFunctionContaining(refAddr);
                if (f == null) {
                    out.println("No function containing this address; trying to disassemble/create one...");
                    DisassembleCommand dis = new DisassembleCommand(refAddr, null, true);
                    dis.applyTo(currentProgram, monitor);
                    CreateFunctionCmd cfc = new CreateFunctionCmd(refAddr);
                    cfc.applyTo(currentProgram, monitor);
                    f = getFunctionContaining(refAddr);
                }
                if (f == null) {
                    out.println("STILL no function found at/around " + refAddr);
                    out.println();
                    continue;
                }
                out.println("Function: " + f.getName() + " @ " + f.getEntryPoint());

                DecompileResults res = decomp.decompileFunction(f, 60, monitor);
                if (res != null && res.decompileCompleted()) {
                    out.println(res.getDecompiledFunction().getC());
                } else {
                    out.println("Decompile failed: " + (res != null ? res.getErrorMessage() : "null result"));
                }

                // Also list callers of this function
                out.println("--- callers of " + f.getName() + " ---");
                ReferenceManager rm = currentProgram.getReferenceManager();
                for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
                    Address fromAddr = r.getFromAddress();
                    Function caller = getFunctionContaining(fromAddr);
                    out.println("  called from " + fromAddr + " in " + (caller != null ? caller.getName() + "@" + caller.getEntryPoint() : "???"));
                }
                out.println();
            }
        } finally {
            decompDone();
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }

    private void decompDone() {}
}
