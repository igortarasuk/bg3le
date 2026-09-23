import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileCaller3 extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_caller3_decomp.txt";
    static final long[] TARGETS = { 0x051528e9L, 0x020beba4L, 0x015c0348L };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);

            for (long addrLong : TARGETS) {
                Address addr = toAddr(addrLong);
                out.println("=== target " + addr + " ===");
                Function f = getFunctionContaining(addr);
                if (f == null) {
                    out.println("no function; attempting create");
                    // try to find real function start by searching backward for a function whose body would include it
                    // fallback: just try to create function AT this exact address
                    DisassembleCommand dis = new DisassembleCommand(addr, null, true);
                    dis.applyTo(currentProgram, monitor);
                    CreateFunctionCmd cfc = new CreateFunctionCmd(addr);
                    cfc.applyTo(currentProgram, monitor);
                    f = getFunctionContaining(addr);
                }
                if (f == null) {
                    out.println("STILL no function");
                    out.println();
                    continue;
                }
                out.println("Function: " + f.getName() + " @ " + f.getEntryPoint() + " bodySize=" + f.getBody().getNumAddresses());
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
                out.println();
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
