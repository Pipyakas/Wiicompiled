using System;
using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class SelfLoopCodeGenTests
{
    [Fact]
    public void CodeGenerator_EmitsSideEffectingInfiniteLoopForDirectSelfJump()
    {
        var function = new IrFunction(
            "self_loop",
            "loc_80166958",
            new[]
            {
                new IrBasicBlock("loc_80166958", new IrInstruction[]
                {
                    new IrJump("loc_80166958")
                })
            });

        var ssa = new SsaTransformer().Convert(function);
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>());
        var signature = new FunctionAbiClassification("self_loop", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(0x80166958, ssa, signature, types);

        Assert.Contains("loc_80166958:", code, StringComparison.Ordinal);
        Assert.Contains("for (;;) { volatile uint32_t guest_spin = 0x80166958u; (void)guest_spin; }", code, StringComparison.Ordinal);
        Assert.DoesNotContain("goto loc_80166958;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_EmitsGotoForConditionalBranchSelfJump()
    {
        var function = new IrFunction(
            "branch_self_loop",
            "loc_80166978",
            new[]
            {
                new IrBasicBlock("loc_80166978", new IrInstruction[]
                {
                    new IrBranch("bdnz", "loc_80166978", "loc_80166980", "ctr")
                }),
                new IrBasicBlock("loc_80166980", new IrInstruction[]
                {
                    new IrReturn(null)
                })
            });

        var ssa = new SsaTransformer().Convert(function);
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>());
        var signature = new FunctionAbiClassification("branch_self_loop", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(0x80166978, ssa, signature, types);

        Assert.Contains("goto loc_80166978;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("guest_spin", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_EmitsZeroGuestSpinForNonAddressSelfJump()
    {
        var function = new IrFunction(
            "symbolic_loop",
            "spin",
            new[]
            {
                new IrBasicBlock("spin", new IrInstruction[]
                {
                    new IrJump("spin")
                })
            });

        var ssa = new SsaTransformer().Convert(function);
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>());
        var signature = new FunctionAbiClassification("symbolic_loop", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(0x80001000, ssa, signature, types);

        Assert.Contains("for (;;) { volatile uint32_t guest_spin = 0x00000000u; (void)guest_spin; }", code, StringComparison.Ordinal);
        Assert.DoesNotContain("goto loc_spin;", code, StringComparison.Ordinal);
    }
}
