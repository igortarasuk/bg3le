import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.util.task.ConsoleTaskMonitor;

public class FindCallers2 extends GhidraScript {
    static final String OUT_PATH =
        "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_modcheck_candidate2.txt";
    static final long IMAGE_BASE = 0x100000L;
    static final long RAW_ENTRY = 0x37675f0L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager refMgr = currentProgram.getReferenceManager();

        Address entry = space.getAddress(RAW_ENTRY + IMAGE_BASE);
        out.println("Entry: " + entry);
        Function func = fm.getFunctionContaining(entry);
        if (func == null) {
            out.println("No function object; creating one.");
            func = createFunction(entry, "GuidOfficialCheck");
        }
        out.println("Function: " + func);

        if (func != null) {
            out.println("Body ranges:");
            func.getBody().forEach(r -> out.println("  " + r));

            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();
            DecompileResults res = decomp.decompileFunction(func, 300, monitor);
            out.println("decompileCompleted(): " + res.decompileCompleted());
            if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
                out.println("--- decompiled C ---");
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("FAILED: " + res.getErrorMessage());
            }
        }

        out.println();
        out.println("=== Callers of entry " + entry + " ===");
        ReferenceIterator refs = refMgr.getReferencesTo(entry);
        int count = 0;
        while (refs.hasNext()) {
            Reference ref = refs.next();
            Address from = ref.getFromAddress();
            Function f = fm.getFunctionContaining(from);
            out.println("  " + from + "  in " + (f != null ? f.getName() + " @ " + f.getEntryPoint() : "???"));
            count++;
        }
        out.println("Total callers: " + count);

        out.close();
        println("FindCallers2: wrote " + OUT_PATH);
    }
}
