import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

import java.io.PrintWriter;

public class DumpFunctionListing extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            throw new IllegalArgumentException("Usage: <function-address> <output-path>");
        }

        Address address = toAddr(args[0]);
        Function function = currentProgram.getFunctionManager().getFunctionContaining(address);
        if (function == null) {
            throw new IllegalArgumentException("No function contains " + address);
        }

        try (PrintWriter output = new PrintWriter(args[1])) {
            output.println(function.getName() + " @ " + function.getEntryPoint());
            InstructionIterator instructions = currentProgram.getListing()
                .getInstructions(function.getBody(), true);
            while (instructions.hasNext() && !monitor.isCancelled()) {
                Instruction instruction = instructions.next();
                output.println(instruction.getAddress() + "  " + instruction);
            }
        }
    }
}
