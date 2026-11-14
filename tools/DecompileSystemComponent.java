// SPDX-License-Identifier: BSD-3-Clause
// Headless postScript: DecompileSystemComponent.java <new-private-output.c> <RVA> [<RVA> ...]
// Use only after identifying a readable original PE. Generated pseudocode and
// proprietary inputs stay outside version control. Maximum: eight functions.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.io.BufferedWriter;

public class DecompileSystemComponent extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2 || args.length > 9)
            throw new IllegalArgumentException("Supply a new output file and one to eight function RVAs");
        if (!currentProgram.getExecutableFormat().equals("Portable Executable (PE)"))
            throw new IllegalArgumentException("A readable PE program is required");
        Path output = Path.of(args[0]).toAbsolutePath();
        Path partial = output.resolveSibling(output.getFileName() + ".partial");
        if (Files.exists(output) || Files.exists(partial))
            throw new IllegalArgumentException("Output must be new; existing evidence is preserved");
        Address[] targets = new Address[args.length - 1];
        for (int i = 1; i < args.length; ++i) {
            long rva = Long.decode(args[i]);
            if (rva < 0 || rva > 0xffffffffL) throw new IllegalArgumentException("Invalid PE RVA");
            Address target = currentProgram.getImageBase().addNoWrap(rva);
            if (getFunctionAt(target) == null)
                throw new IllegalArgumentException("No analyzed function starts at " + target);
            targets[i - 1] = target;
        }
        DecompInterface decompiler = new DecompInterface();
        try {
            if (!decompiler.openProgram(currentProgram))
                throw new IllegalStateException(decompiler.getLastMessage());
            try (BufferedWriter writer = Files.newBufferedWriter(partial, StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
                writer.write("/* Recovered pseudocode, not original source or a buildable program.\n");
                writer.write("Input: " + currentProgram.getName() + "\nSHA256: " + currentProgram.getExecutableSHA256());
                writer.write("\nLanguage: " + currentProgram.getLanguageID() + "\nImage base: " + currentProgram.getImageBase() + " */\n");
                for (Address target : targets) {
                    monitor.checkCancelled();
                    Function function = getFunctionAt(target);
                    DecompileResults result = decompiler.decompileFunction(function, 15, monitor);
                    if (!result.decompileCompleted() || result.getDecompiledFunction() == null)
                        throw new IllegalStateException("Incomplete decompilation at " + target + ": " + result.getErrorMessage());
                    String code = result.getDecompiledFunction().getC();
                    if (code.length() > 32768) throw new IllegalStateException("Function exceeds output budget at " + target);
                    writer.write("\n/* Address: " + target + "; RVA: 0x" +
                        Long.toHexString(target.subtract(currentProgram.getImageBase())) + " */\n");
                    writer.write(code);
                }
                writer.write("\n/* DECOMPILATION_COMPLETE functions=" + targets.length + " */\n");
            }
            Files.move(partial, output);
            println("DECOMPILATION_COMPLETE sha256=" + currentProgram.getExecutableSHA256() +
                " functions=" + targets.length + " output=" + output);
        } finally {
            decompiler.dispose();
        }
    }
}
