import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Function;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

public class FindOffsetWrites extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_offset_writes.txt";
    static final int[] OFFSETS = { 0x248, 0x260, 0x278 };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            Memory mem = currentProgram.getMemory();
            Map<Integer, Set<String>> offsetToFuncs = new HashMap<>();
            Map<Integer, List<String>> offsetToHits = new HashMap<>();

            for (int offset : OFFSETS) {
                byte[] pattern = new byte[] {
                    (byte) (offset & 0xff), (byte) ((offset >> 8) & 0xff), 0, 0
                };
                Set<String> funcs = new HashSet<>();
                List<String> hitsList = new ArrayList<>();
                for (MemoryBlock block : mem.getBlocks()) {
                    if (!block.isExecute() || !block.isInitialized()) continue;
                    Address start = block.getStart();
                    Address end = block.getEnd();
                    Address searchFrom = start;
                    int hitCount = 0;
                    while (true) {
                        Address found = mem.findBytes(searchFrom, end, pattern, null, true, monitor);
                        if (found == null) break;
                        hitCount++;
                        // check instruction containing this disp32 (disp32 is usually 2-6 bytes before/after opcode start;
                        // scan a small window before 'found' for an instruction whose bytes span this address)
                        for (int back = 1; back <= 7; back++) {
                            Address insStart = found.subtract(back);
                            Instruction ins = getInstructionAt(insStart);
                            if (ins != null && ins.getLength() >= back + 4) {
                                String mnem = ins.getMnemonicString();
                                String rep = ins.toString();
                                if ((mnem.startsWith("MOV") || mnem.startsWith("SETcc") || mnem.startsWith("SET") || mnem.startsWith("AND") || mnem.startsWith("OR") || mnem.startsWith("XOR"))
                                        && rep.contains("0x" + Integer.toHexString(offset))) {
                                    Function f = getFunctionContaining(insStart);
                                    String fname = f != null ? f.getName() + "@" + f.getEntryPoint() : "???@" + insStart;
                                    funcs.add(fname);
                                    hitsList.add(insStart + ": " + rep + "  [fn=" + fname + "]");
                                }
                                break;
                            }
                        }
                        try {
                            searchFrom = found.add(1);
                        } catch (Exception e) { break; }
                        if (searchFrom.compareTo(end) >= 0) break;
                        if (hitCount > 20000) break; // safety
                    }
                }
                offsetToFuncs.put(offset, funcs);
                offsetToHits.put(offset, hitsList);
                out.println("offset 0x" + Integer.toHexString(offset) + ": " + funcs.size() + " distinct candidate functions, " + hitsList.size() + " instruction hits");
            }

            // intersect
            Set<String> common = new HashSet<>(offsetToFuncs.get(OFFSETS[0]));
            for (int i = 1; i < OFFSETS.length; i++) {
                common.retainAll(offsetToFuncs.get(OFFSETS[i]));
            }
            out.println();
            out.println("=== Functions writing to ALL of " + Arrays.toString(OFFSETS) + " ===");
            for (String s : common) out.println("  " + s);

            out.println();
            for (int offset : OFFSETS) {
                out.println("--- raw hits for offset 0x" + Integer.toHexString(offset) + " ---");
                for (String h : offsetToHits.get(offset)) out.println("  " + h);
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
