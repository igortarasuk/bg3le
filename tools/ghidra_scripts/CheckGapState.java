import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

public class CheckGapState extends GhidraScript {
    static final String OUT_PATH =
        "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_modcheck_candidate.txt";

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        FunctionManager fm = currentProgram.getFunctionManager();

        Function func = fm.getFunctionAt(space.getAddress(0x02f06590L));
        out.println("Function: " + func);
        if (func != null) {
            out.println("Body ranges now:");
            func.getBody().forEach(r -> out.println("  " + r));
        }

        out.println();
        out.println("--- window around 0x02f07a89 (jnz target) ---");
        Address cur = space.getAddress(0x02f07a00L);
        Address end = space.getAddress(0x02f07c00L);
        while (cur != null && cur.compareTo(end) < 0) {
            Instruction ins = currentProgram.getListing().getInstructionAt(cur);
            if (ins == null) {
                out.println("  " + cur + "  (no instruction)");
                cur = cur.add(1);
            } else {
                out.println("  " + cur + "  " + ins);
                cur = cur.add(ins.getLength());
            }
        }
        out.close();
        println("CheckGapState: wrote " + OUT_PATH);
    }
}
