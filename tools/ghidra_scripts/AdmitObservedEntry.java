// Admits entry points that Ghidra's analysis misses before the export: code reached only through vtables, jump
// tables, code pointers in data or immediates. Arguments: a comma-separated list of specifications and the module's
// SHA-256.
//   repair:FIRST:LAST    clear and disassemble a range that an earlier analysis decoded wrongly
//   table:FIRST:END      every non-zero dword in [FIRST, END) is an entry
//   entries-file:PATH    one RVA per line (# starts a comment)
//   immediates           executable addresses used as instruction immediates
//   vtables              runs of at least two code pointers in non-executable blocks
//   RVA                  a single entry
// @category Sacred

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

public class AdmitObservedEntry extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2 || !args[1].equals(currentProgram.getExecutableSHA256())) {
            throw new IllegalArgumentException("Expected RVA and matching original module SHA-256");
        }

        var targets = new LinkedHashSet<Long>();
        var vtableTargets = new TreeSet<Long>();
        var immediateTargets = new TreeSet<Long>();
        var pointerEntries = new TreeSet<Long>();
        var base = currentProgram.getImageBase();

        for (String specification : args[0].split(",")) {
            if (specification.startsWith("repair:")) {
                String[] bounds = specification.substring(7).split(":");
                var first = base.add(Long.decode(bounds[0]));
                var last = base.add(Long.decode(bounds[1]));
                clearListing(first, last);
                targets.add(first.getOffset());
                println("OBSERVED_DECODE_REPAIR " + first + ".." + last);
            } else if (specification.startsWith("table:")) {
                String[] bounds = specification.substring(6).split(":");
                long first = Long.decode(bounds[0]);
                long end = Long.decode(bounds[1]);
                if (end < first || (end - first) % 4 != 0) {
                    throw new IllegalArgumentException("Invalid initializer table bounds");
                }

                for (long at = first; at < end; at += 4) {
                    long target = Integer.toUnsignedLong(currentProgram.getMemory().getInt(base.add(at)));
                    if (target != 0) {
                        targets.add(target);
                    }
                }
            } else if (specification.startsWith("entries-file:")) {
                for (String line : Files.readAllLines(Path.of(specification.substring(13)))) {
                    String entry = line.split("#")[0].trim();
                    if (!entry.isEmpty()) {
                        pointerEntries.add(base.getOffset() + Long.decode(entry.split("\\s+")[0]));
                    }
                }
            } else if (specification.equals("immediates")) {
                collectImmediates(base.getOffset(), immediateTargets);
            } else if (specification.equals("vtables")) {
                collectVtables(vtableTargets, immediateTargets);
            } else {
                targets.add(base.getOffset() + Long.decode(specification));
            }
        }

        admitObserved(targets);
        admitVtableEntries(vtableTargets);
        admitImmediateEntries(immediateTargets);
        admitPointerEntries(pointerEntries);
    }

    private void collectImmediates(long base, Set<Long> immediateTargets) throws Exception {
        var memory = currentProgram.getMemory();
        for (var instruction : currentProgram.getListing().getInstructions(true)) {
            for (int operand = 0; operand < instruction.getNumOperands(); ++operand) {
                var scalar = instruction.getScalar(operand);
                if (scalar == null) {
                    continue;
                }

                long value = scalar.getUnsignedValue();
                if (value < base || value > 0xffffffffL) {
                    continue;
                }

                var holder = memory.getBlock(toAddr(value));
                if (holder != null && holder.isExecute()) {
                    immediateTargets.add(value);
                }
            }
        }
    }

    private void collectVtables(Set<Long> vtableTargets, Set<Long> immediateTargets) throws Exception {
        var memory = currentProgram.getMemory();
        for (var block : memory.getBlocks()) {
            if (!block.isInitialized() || block.isExecute()) {
                continue;
            }

            List<Long> run = new ArrayList<>();
            long end = block.getEnd().getOffset();
            for (long at = block.getStart().getOffset(); at + 4 <= end + 1; at += 4) {
                long value = Integer.toUnsignedLong(memory.getInt(toAddr(at)));
                var holder = value == 0 ? null : memory.getBlock(toAddr(value));
                if (holder != null && holder.isExecute()) {
                    run.add(value);
                    continue;
                }

                flushRun(run, vtableTargets, immediateTargets);
            }

            flushRun(run, vtableTargets, immediateTargets);
        }
    }

    // Two or more code pointers in a row are taken as a vtable; a single one is only a candidate.
    private static void flushRun(List<Long> run, Set<Long> vtableTargets, Set<Long> immediateTargets) {
        if (run.size() >= 2) {
            vtableTargets.addAll(run);
        } else {
            immediateTargets.addAll(run);
        }

        run.clear();
    }

    private void admitObserved(Set<Long> targets) throws Exception {
        for (long target : targets) {
            var address = toAddr(target);
            if (!currentProgram.getMemory().getBlock(address).isExecute()) {
                throw new IllegalArgumentException("Observed entry is not in an executable section");
            }

            // An earlier analysis may have typed code bytes as data; disassemble() would then silently do nothing.
            var data = getInstructionAt(address) == null ? getDataContaining(address) : null;
            if (data != null) {
                clearListing(data.getMinAddress(), data.getMaxAddress());
                println("OBSERVED_DATA_CLEARED " + data.getMinAddress() + ".." + data.getMaxAddress());
            }

            if (getInstructionAt(address) == null && !disassemble(address)) {
                throw new IllegalStateException("Cannot disassemble observed entry " + address);
            }

            if (getFunctionContaining(address) == null && createFunction(address, null) == null) {
                var containing = getInstructionContaining(address);
                throw new IllegalStateException("Cannot create observed function " + address
                        + " containing instruction " + containing + " at "
                        + (containing == null ? null : containing.getAddress()));
            }

            println("OBSERVED_ENTRY_ADMITTED " + address);
        }
    }

    private void admitVtableEntries(Set<Long> vtableTargets) throws Exception {
        int admitted = 0;
        int skipped = 0;
        for (long target : vtableTargets) {
            var address = toAddr(target);
            if (getFunctionContaining(address) != null) {
                continue;
            }

            if (splitsInstruction(address) || (getInstructionAt(address) == null && !disassemble(address))
                    || createFunction(address, null) == null) {
                ++skipped;
                continue;
            }

            ++admitted;
        }

        println("VTABLE_ENTRIES_ADMITTED " + admitted + " skipped " + skipped);
    }

    private void admitImmediateEntries(Set<Long> immediateTargets) throws Exception {
        var memory = currentProgram.getMemory();
        int admitted = 0;
        int rejected = 0;
        for (long target : immediateTargets) {
            var address = toAddr(target);
            if (getFunctionContaining(address) != null || getInstructionContaining(address) != null) {
                continue;
            }

            // Only after padding (int3, nop), a ret or a ret imm16.
            int previous = memory.getByte(address.subtract(1)) & 0xff;
            int before = memory.getByte(address.subtract(3)) & 0xff;
            if (previous != 0xcc && previous != 0x90 && previous != 0xc3 && before != 0xc2) {
                ++rejected;
                continue;
            }

            if (!disassemble(address) || createFunction(address, null) == null) {
                ++rejected;
                continue;
            }

            ++admitted;
        }

        println("IMMEDIATE_ENTRIES_ADMITTED " + admitted + " rejected " + rejected);
    }

    private void admitPointerEntries(Set<Long> pointerEntries) throws Exception {
        int admitted = 0;
        int existing = 0;
        int refused = 0;
        for (long target : pointerEntries) {
            var address = toAddr(target);
            var block = currentProgram.getMemory().getBlock(address);
            if (block == null || !block.isExecute()) {
                ++refused;
                continue;
            }

            if (getFunctionContaining(address) != null && getInstructionAt(address) != null) {
                ++existing;
                continue;
            }

            if (splitsInstruction(address)) {
                ++refused;
                println("CODE_POINTER_ENTRY_REFUSED " + address + " splits an instruction");
                continue;
            }

            var data = getInstructionAt(address) == null ? getDataContaining(address) : null;
            if (data != null) {
                clearListing(data.getMinAddress(), data.getMaxAddress());
            }

            boolean decoded = getInstructionAt(address) != null || disassemble(address);
            if (!decoded || (getFunctionContaining(address) == null && createFunction(address, null) == null)) {
                ++refused;
                println("CODE_POINTER_ENTRY_REFUSED " + address + " does not decode as a function");
                continue;
            }

            ++admitted;
        }

        println("CODE_POINTER_ENTRIES_ADMITTED " + admitted + " existing " + existing + " refused " + refused);
    }

    private boolean splitsInstruction(Address address) {
        return getInstructionContaining(address) != null && getInstructionAt(address) == null;
    }
}
