#include "qv4isel_util_p.h"
#include "qv4isel_moth_p.h"
#include "qv4vme_moth_p.h"
#include "debugging.h"

using namespace QQmlJS;
using namespace QQmlJS::Moth;

namespace {

QTextStream qout(stderr, QIODevice::WriteOnly);

static unsigned toValueOrTemp(IR::Expr *e, Instr::ValueOrTemp &vot)
{
    if (IR::Const *c = e->asConst()) {
        vot.value = convertToValue(c);
        return 0;
    } else if (IR::Temp *t = e->asTemp()) {
        vot.tempIndex = t->index;
        return 1;
    } else {
        Q_UNREACHABLE();
    }
}

#undef DEBUG_TEMP_COMPRESSION
class CompressTemps: public IR::StmtVisitor, IR::ExprVisitor
{
public:
    void run(IR::Function *function)
    {
        _seenTemps.clear();
        _nextFree = 0;
        _active.reserve(function->tempCount);
        _localCount = function->locals.size();
        pinReturnTemp(function);
        int maxUsed = _nextFree;

        foreach (IR::BasicBlock *block, function->basicBlocks) {
#ifdef DEBUG_TEMP_COMPRESSION
            qDebug("L%d:", block->index);
#endif // DEBUG_TEMP_COMPRESSION

            for (int i = 0, ei = block->statements.size(); i < ei; ++i ) {
                _currentStatement = block->statements[i];
                if (i == 0)
                    expireOld();

#ifdef DEBUG_TEMP_COMPRESSION
                _currentStatement->dump(qout);qout<<endl<<flush;
#endif // DEBUG_TEMP_COMPRESSION

                if (_currentStatement->d)
                    _currentStatement->accept(this);
            }
            maxUsed = std::max(maxUsed, _nextFree);
        }
#ifdef DEBUG_TEMP_COMPRESSION
        qDebug() << "function" << (*function->name) << "uses" << maxUsed << "temps.";
#endif // DEBUG_TEMP_COMPRESSION
        function->tempCount = maxUsed + _localCount;
    }

private:
    void pinReturnTemp(IR::Function *function) {
        const IR::BasicBlock *returnBlock = function->basicBlocks.last();
        assert(returnBlock);
        IR::Ret *ret = returnBlock->terminator()->asRet();
        assert(ret);
        IR::Temp *t = ret->expr->asTemp();
        assert(t);
        assert(t->index >= 0);
        _pinnedReturnValue = _nextFree;
        add(t->index, _pinnedReturnValue);
    }

    virtual void visitConst(IR::Const *) {}
    virtual void visitString(IR::String *) {}
    virtual void visitRegExp(IR::RegExp *) {}
    virtual void visitName(IR::Name *) {}
    virtual void visitClosure(IR::Closure *) {}
    virtual void visitUnop(IR::Unop *e) { e->expr->accept(this); }
    virtual void visitBinop(IR::Binop *e) { e->left->accept(this); e->right->accept(this); }
    virtual void visitSubscript(IR::Subscript *e) { e->base->accept(this); e->index->accept(this); }
    virtual void visitMember(IR::Member *e) { e->base->accept(this); }
    virtual void visitExp(IR::Exp *s) { s->expr->accept(this); }
    virtual void visitEnter(IR::Enter *) {}
    virtual void visitLeave(IR::Leave *) {}
    virtual void visitJump(IR::Jump *) {}
    virtual void visitCJump(IR::CJump *s) { s->cond->accept(this); }
    virtual void visitRet(IR::Ret *s) { s->expr->accept(this); }

    virtual void visitTemp(IR::Temp *e) {
        if (_seenTemps.contains(e))
            return;
        _seenTemps.insert(e);

        if (e->index < 0)
            return;
        if (e->index < _localCount) // don't optimise locals yet.
            return;

        e->index = remap(e->index - _localCount) + _localCount;
    }

    virtual void visitCall(IR::Call *e) {
        e->base->accept(this);
        for (IR::ExprList *it = e->args; it; it = it->next)
            it->expr->accept(this);
    }

    virtual void visitNew(IR::New *e) {
        e->base->accept(this);
        for (IR::ExprList *it = e->args; it; it = it->next)
            it->expr->accept(this);
    }

    virtual void visitMove(IR::Move *s) {
        s->target->accept(this);
        s->source->accept(this);
    }

    int remap(int tempIndex) {
        for (ActiveTemps::const_iterator i = _active.begin(), ei = _active.end(); i < ei; ++i) {
            if (i->first == tempIndex) {
#ifdef DEBUG_TEMP_COMPRESSION
                qDebug() << "    lookup" << (tempIndex + _localCount) << "->" << (i->second + _localCount);
#endif // DEBUG_TEMP_COMPRESSION
                return i->second;
            }
        }

        int firstFree = expireOld();
        add(tempIndex, firstFree);
        return firstFree;
    }

    void add(int tempIndex, int firstFree) {
        if (_nextFree <= firstFree)
            _nextFree = firstFree + 1;
        _active.prepend(qMakePair(tempIndex, firstFree));
#ifdef DEBUG_TEMP_COMPRESSION
        qDebug() << "    add" << (tempIndex + _localCount) << "->" << (firstFree+ _localCount);
#endif // DEBUG_TEMP_COMPRESSION
    }

    int expireOld() {
        Q_ASSERT(_currentStatement->d);

        const QBitArray &liveIn = _currentStatement->d->liveIn;
        QBitArray inUse(_nextFree);
        int i = 0;
        while (i < _active.size()) {
            const QPair<int, int> &p = _active[i];

            if (p.second == _pinnedReturnValue) {
                inUse.setBit(p.second);
                ++i;
                continue;
            }

            if (liveIn[p.first + _localCount]) {
                inUse[p.second] = true;
                ++i;
            } else {
#ifdef DEBUG_TEMP_COMPRESSION
                qDebug() << "    remove" << (p.first + _localCount) << "->" << (p.second + _localCount);
#endif // DEBUG_TEMP_COMPRESSION
                _active.remove(i);
            }
        }
        for (int i = 0, ei = inUse.size(); i < ei; ++i)
            if (!inUse[i])
                return i;
        return _nextFree;
    }

private:
    typedef QVector<QPair<int, int> > ActiveTemps;
    ActiveTemps _active;
    QSet<IR::Temp *> _seenTemps;
    IR::Stmt *_currentStatement;
    int _localCount;
    int _nextFree;
    int _pinnedReturnValue;
};

} // anonymous namespace

InstructionSelection::InstructionSelection(VM::ExecutionEngine *engine, IR::Module *module)
    : EvalInstructionSelection(engine, module)
    , _function(0)
    , _block(0)
    , _code(0)
    , _ccode(0)
{
}

InstructionSelection::~InstructionSelection()
{
}

void InstructionSelection::run(VM::Function *vmFunction, IR::Function *function)
{
    qSwap(_function, function);

    IR::BasicBlock *block;

    QHash<IR::BasicBlock *, QVector<ptrdiff_t> > patches;
    QHash<IR::BasicBlock *, ptrdiff_t> addrs;

    // FIXME: make the size dynamic. This requires changing the patching.
    uchar *code = new uchar[getpagesize() * 4000];
    uchar *ccode = code;

    qSwap(block, _block);
    qSwap(patches, _patches);
    qSwap(addrs, _addrs);
    qSwap(code, _code);
    qSwap(ccode, _ccode);

    CompressTemps().run(_function);

    int locals = frameSize();
    assert(locals >= 0);

    Instruction::Push push;
    push.value = quint32(locals);
    addInstruction(push);

    foreach (_block, _function->basicBlocks) {
        _addrs.insert(_block, _ccode - _code);

        foreach (IR::Stmt *s, _block->statements)
            s->accept(this);
    }

    for (QHash<IR::BasicBlock *, QVector<ptrdiff_t> >::ConstIterator iter = _patches.begin();
         iter != _patches.end(); ++iter) {

        Q_ASSERT(_addrs.contains(iter.key()));
        ptrdiff_t target = _addrs.value(iter.key());

        const QVector<ptrdiff_t> &patchList = iter.value();
        for (int ii = 0; ii < patchList.count(); ++ii) {
            ptrdiff_t patch = patchList.at(ii);

            *((ptrdiff_t *)(_code + patch)) = target - patch;
        }
    }

    qSwap(_function, function);
    _patches.clear();
    _addrs.clear();

    vmFunction->code = VME::exec;
    vmFunction->codeData = _code;

    qSwap(block, _block);
    qSwap(patches, _patches);
    qSwap(addrs, _addrs);
    qSwap(code, _code);
    qSwap(ccode, _ccode);
}

void InstructionSelection::callActivationProperty(IR::Call *c, int targetTempIndex)
{
    IR::Name *baseName = c->base->asName();
    Q_ASSERT(baseName);

    switch (baseName->builtin) {
    case IR::Name::builtin_invalid: {
        const int scratchIndex = scratchTempIndex();

        Instruction::LoadName load;
        load.name = engine()->newString(*baseName->id);
        load.targetTempIndex = scratchIndex;
        addInstruction(load);

        Instruction::CallValue call;
        prepareCallArgs(c->args, call.argc, call.args);
        call.destIndex = scratchIndex;
        call.targetTempIndex = targetTempIndex;
        addInstruction(call);
    } break;

    case IR::Name::builtin_typeof: {
        if (IR::Member *m = c->args->expr->asMember()) {
            Instruction::CallBuiltinTypeofMember call;
            call.base = m->base->asTemp()->index;
            call.member = engine()->identifier(*m->name);
            call.targetTempIndex = targetTempIndex;
            addInstruction(call);
        } else if (IR::Subscript *ss = c->args->expr->asSubscript()) {
            Instruction::CallBuiltinTypeofSubscript call;
            call.base = ss->base->asTemp()->index;
            call.index = ss->index->asTemp()->index;
            call.targetTempIndex = targetTempIndex;
            addInstruction(call);
        } else if (IR::Name *n = c->args->expr->asName()) {
            Instruction::CallBuiltinTypeofName call;
            call.name = engine()->identifier(*n->id);
            call.targetTempIndex = targetTempIndex;
            addInstruction(call);
        } else if (IR::Temp *arg = c->args->expr->asTemp()){
            assert(arg != 0);
            Instruction::CallBuiltinTypeofValue call;
            call.tempIndex = arg->index;
            call.targetTempIndex = targetTempIndex;
            addInstruction(call);
        } else {
            assert(false);
        }
    } break;

    case IR::Name::builtin_delete: {
        if (IR::Member *m = c->args->expr->asMember()) {
            Instruction::CallBuiltinDeleteMember call;
            call.base = m->base->asTemp()->index;
            call.member = engine()->newString(*m->name);
            call.targetTempIndex = targetTempIndex;
            addInstruction(call);
        } else if (IR::Subscript *ss = c->args->expr->asSubscript()) {
            Instruction::CallBuiltinDeleteSubscript call;
            call.base = ss->base->asTemp()->index;
            call.index = ss->index->asTemp()->index;
            call.targetTempIndex = targetTempIndex;
            addInstruction(call);
        } else if (IR::Name *n = c->args->expr->asName()) {
            Instruction::CallBuiltinDeleteName call;
            call.name = engine()->newString(*n->id);
            call.targetTempIndex = targetTempIndex;
            addInstruction(call);
        } else {
            Instruction::CallBuiltinDeleteValue call;
            call.tempIndex = c->args->expr->asTemp()->index;
            call.targetTempIndex = targetTempIndex;
            addInstruction(call);
        }
    } break;

    case IR::Name::builtin_throw: {
        IR::Temp *arg = c->args->expr->asTemp();
        assert(arg != 0);

        Instruction::CallBuiltin call;
        call.builtin = Instruction::CallBuiltin::builtin_throw;
        prepareCallArgs(c->args, call.argc, call.args);
        call.targetTempIndex = targetTempIndex;
        addInstruction(call);
    } break;

    case IR::Name::builtin_rethrow: {
        Instruction::CallBuiltin call;
        call.builtin = Instruction::CallBuiltin::builtin_rethrow;
        call.targetTempIndex = targetTempIndex;
        addInstruction(call);
    } break;

    case IR::Name::builtin_create_exception_handler: {
        Instruction::CallBuiltin call;
        call.builtin = Instruction::CallBuiltin::builtin_create_exception_handler;
        call.targetTempIndex = targetTempIndex;
        addInstruction(call);
    } break;

    case IR::Name::builtin_delete_exception_handler: {
        Instruction::CallBuiltin call;
        call.builtin = Instruction::CallBuiltin::builtin_delete_exception_handler;
        call.targetTempIndex = targetTempIndex;
        addInstruction(call);
    } break;

    case IR::Name::builtin_get_exception: {
        Instruction::CallBuiltin call;
        call.builtin = Instruction::CallBuiltin::builtin_get_exception;
        call.targetTempIndex = targetTempIndex;
        addInstruction(call);
    } break;

    case IR::Name::builtin_foreach_iterator_object: {
        Instruction::CallBuiltin call;
        call.builtin = Instruction::CallBuiltin::builtin_foreach_iterator_object;
        prepareCallArgs(c->args, call.argc, call.args);
        call.targetTempIndex = targetTempIndex;
        addInstruction(call);
    } break;

    case IR::Name::builtin_foreach_next_property_name: {
        Instruction::CallBuiltin call;
        call.builtin = Instruction::CallBuiltin::builtin_foreach_next_property_name;
        prepareCallArgs(c->args, call.argc, call.args);
        call.targetTempIndex = targetTempIndex;
        addInstruction(call);
    } break;

    case IR::Name::builtin_push_with: {
        Instruction::CallBuiltin call;
        call.builtin = Instruction::CallBuiltin::builtin_push_with;
        prepareCallArgs(c->args, call.argc, call.args);
        assert(call.argc == 1);
        addInstruction(call);
    } break;

    case IR::Name::builtin_pop_with: {
        Instruction::CallBuiltin call;
        call.builtin = Instruction::CallBuiltin::builtin_pop_with;
        addInstruction(call);
    } break;

    case IR::Name::builtin_declare_vars: if (c->args) {
        IR::Const *deletable = c->args->expr->asConst();
        assert(deletable->type == IR::BoolType);
        const bool isDeletable = deletable->value != 0;
        for (IR::ExprList *it = c->args->next; it; it = it->next) {
            Instruction::CallBuiltinDeclareVar call;
            call.isDeletable = isDeletable;
            call.varName = engine()->newString(*it->expr->asName()->id);
            addInstruction(call);
        }
    } break;

    default:
        Q_UNIMPLEMENTED();
        c->dump(qout); qout << endl;
    }
}

void InstructionSelection::callValue(IR::Call *c, int targetTempIndex)
{
    IR::Temp *t = c->base->asTemp();
    Q_ASSERT(t);

    Instruction::CallValue call;
    prepareCallArgs(c->args, call.argc, call.args);
    call.destIndex = t->index;
    call.targetTempIndex = targetTempIndex;
    addInstruction(call);
}

void InstructionSelection::callProperty(IR::Call *c, int targetTempIndex)
{
    IR::Member *m = c->base->asMember();
    Q_ASSERT(m);

    // call the property on the loaded base
    Instruction::CallProperty call;
    call.baseTemp = m->base->asTemp()->index;
    call.name = engine()->newString(*m->name);
    prepareCallArgs(c->args, call.argc, call.args);
    call.targetTempIndex = targetTempIndex;
    addInstruction(call);
}

void InstructionSelection::construct(IR::New *ctor, int targetTempIndex)
{
    if (IR::Name *baseName = ctor->base->asName()) {
        Instruction::CreateActivationProperty create;
        create.name = engine()->newString(*baseName->id);
        prepareCallArgs(ctor->args, create.argc, create.args);
        create.targetTempIndex = targetTempIndex;
        addInstruction(create);
    } else if (IR::Member *member = ctor->base->asMember()) {
        IR::Temp *base = member->base->asTemp();
        assert(base != 0);

        Instruction::CreateProperty create;
        create.base = base->index;
        create.name = engine()->newString(*member->name);
        prepareCallArgs(ctor->args, create.argc, create.args);
        create.targetTempIndex = targetTempIndex;
        addInstruction(create);
    } else if (IR::Temp *baseTemp = ctor->base->asTemp()) {
        Instruction::CreateValue create;
        create.func = baseTemp->index;
        prepareCallArgs(ctor->args, create.argc, create.args);
        create.targetTempIndex = targetTempIndex;
        addInstruction(create);
    } else {
        qWarning("  NEW");
    }
}

void InstructionSelection::prepareCallArgs(IR::ExprList *e, quint32 &argc, quint32 &args)
{
    argc = 0;
    args = 0;

    bool singleArgIsTemp = false;
    if (e && e->next == 0) {
        // ok, only 1 argument in the cal...
        const int idx = e->expr->asTemp()->index;
        if (idx >= 0) {
            // not an argument to this function...
            // so if it's not a local, we're in:
            singleArgIsTemp = idx >= _function->locals.size();
        }
    }

    if (singleArgIsTemp) {
        // We pass single arguments as references to the stack, but only if it's not a local or an argument.
        argc = 1;
        args = e->expr->asTemp()->index;
    } else if (e) {
        // We need to move all the temps into the function arg array
        int argLocation = outgoingArgumentTempStart();
        assert(argLocation >= 0);
        args = argLocation;
        while (e) {
            Instruction::MoveTemp move;
            move.fromTempIndex = e->expr->asTemp()->index;
            move.toTempIndex = argLocation;
            addInstruction(move);
            ++argLocation;
            ++argc;
            e = e->next;
        }
    }
}

void InstructionSelection::visitExp(IR::Exp *s)
{
    if (IR::Call *c = s->expr->asCall()) {
        // These are calls where the result is ignored.
        const int targetTempIndex = scratchTempIndex();
        if (c->base->asName()) {
            callActivationProperty(c, targetTempIndex);
        } else if (c->base->asTemp()) {
            callValue(c, targetTempIndex);
        } else if (c->base->asMember()) {
            callProperty(c, targetTempIndex);
        } else {
            Q_UNREACHABLE();
        }
    } else {
        Q_UNREACHABLE();
    }
}

void InstructionSelection::visitEnter(IR::Enter *)
{
    qWarning("%s", __PRETTY_FUNCTION__);
    Q_UNREACHABLE();
}

void InstructionSelection::visitLeave(IR::Leave *)
{
    qWarning("%s", __PRETTY_FUNCTION__);
    Q_UNREACHABLE();
}

typedef VM::Value (*ALUFunction)(const VM::Value, const VM::Value, VM::ExecutionContext*);
static inline ALUFunction aluOpFunction(IR::AluOp op)
{
    switch (op) {
    case IR::OpInvalid:
        return 0;
    case IR::OpIfTrue:
        return 0;
    case IR::OpNot:
        return 0;
    case IR::OpUMinus:
        return 0;
    case IR::OpUPlus:
        return 0;
    case IR::OpCompl:
        return 0;
    case IR::OpBitAnd:
        return VM::__qmljs_bit_and;
    case IR::OpBitOr:
        return VM::__qmljs_bit_or;
    case IR::OpBitXor:
        return VM::__qmljs_bit_xor;
    case IR::OpAdd:
        return VM::__qmljs_add;
    case IR::OpSub:
        return VM::__qmljs_sub;
    case IR::OpMul:
        return VM::__qmljs_mul;
    case IR::OpDiv:
        return VM::__qmljs_div;
    case IR::OpMod:
        return VM::__qmljs_mod;
    case IR::OpLShift:
        return VM::__qmljs_shl;
    case IR::OpRShift:
        return VM::__qmljs_shr;
    case IR::OpURShift:
        return VM::__qmljs_ushr;
    case IR::OpGt:
        return VM::__qmljs_gt;
    case IR::OpLt:
        return VM::__qmljs_lt;
    case IR::OpGe:
        return VM::__qmljs_ge;
    case IR::OpLe:
        return VM::__qmljs_le;
    case IR::OpEqual:
        return VM::__qmljs_eq;
    case IR::OpNotEqual:
        return VM::__qmljs_ne;
    case IR::OpStrictEqual:
        return VM::__qmljs_se;
    case IR::OpStrictNotEqual:
        return VM::__qmljs_sne;
    case IR::OpInstanceof:
        return VM::__qmljs_instanceof;
    case IR::OpIn:
        return VM::__qmljs_in;
    case IR::OpAnd:
        return 0;
    case IR::OpOr:
        return 0;
    default:
        assert(!"Unknown AluOp");
        return 0;
    }
};

void InstructionSelection::visitMove(IR::Move *s)
{
    if (IR::Temp *t = s->target->asTemp()) {
        const int targetTempIndex = t->index;
        // Check what kind of load it is, and generate the instruction for that.
        // The store to the temp (the target) is done afterwards.
        if (IR::Name *n = s->source->asName()) {
            Q_UNUSED(n);
            if (*n->id == QStringLiteral("this")) { // ### `this' should be a builtin.
                Instruction::LoadThis load;
                load.targetTempIndex = targetTempIndex;
                addInstruction(load);
            } else {
                Instruction::LoadName load;
                load.name = engine()->newString(*n->id);
                load.targetTempIndex = targetTempIndex;
                addInstruction(load);
            }
        } else if (s->source->asTemp() || s->source->asConst()) {
            if (s->op == IR::OpInvalid) {
                if (IR::Temp *t2 = s->source->asTemp()) {
                    Instruction::MoveTemp move;
                    move.fromTempIndex = t2->index;
                    move.toTempIndex = targetTempIndex;
                    addInstruction(move);
                } else {
                    IR::Const *c = s->source->asConst();
                    assert(c);
                    Instruction::LoadValue load;
                    load.targetTempIndex = targetTempIndex;
                    load.value = convertToValue(c);
                    addInstruction(load);
                }
            } else {
                Instruction::Binop binop;
                binop.alu = aluOpFunction(s->op);
                binop.lhsIsTemp = toValueOrTemp(t, binop.lhs);
                binop.rhsIsTemp = toValueOrTemp(s->source, binop.rhs);
                binop.targetTempIndex = targetTempIndex;
                addInstruction(binop);
            }
        } else if (IR::String *str = s->source->asString()) {
            Instruction::LoadValue load;
            load.value = VM::Value::fromString(engine()->newString(*str->value));
            load.targetTempIndex = targetTempIndex;
            addInstruction(load);
        } else if (IR::Closure *clos = s->source->asClosure()) {
            VM::Function *vmFunc = vmFunction(clos->value);
            assert(vmFunc);
            Instruction::LoadClosure load;
            load.value = vmFunc;
            load.targetTempIndex = targetTempIndex;
            addInstruction(load);
        } else if (IR::New *ctor = s->source->asNew()) {
            construct(ctor, targetTempIndex);
        } else if (IR::Member *m = s->source->asMember()) {
            if (IR::Temp *base = m->base->asTemp()) {
                Instruction::LoadProperty load;
                load.baseTemp = base->index;
                load.name = engine()->newString(*m->name);
                load.targetTempIndex = targetTempIndex;
                addInstruction(load);
            } else {
                qWarning("  MEMBER");
            }
        } else if (IR::Subscript *ss = s->source->asSubscript()) {
            Instruction::LoadElement load;
            load.base = ss->base->asTemp()->index;
            load.index = ss->index->asTemp()->index;
            load.targetTempIndex = targetTempIndex;
            addInstruction(load);
        } else if (IR::Unop *u = s->source->asUnop()) {
            if (IR::Temp *e = u->expr->asTemp()) {
                VM::Value (*op)(const VM::Value value, VM::ExecutionContext *ctx) = 0;
                switch (u->op) {
                case IR::OpIfTrue: assert(!"unreachable"); break;
                case IR::OpNot: op = VM::__qmljs_not; break;
                case IR::OpUMinus: op = VM::__qmljs_uminus; break;
                case IR::OpUPlus: op = VM::__qmljs_uplus; break;
                case IR::OpCompl: op = VM::__qmljs_compl; break;
                case IR::OpIncrement: op = VM::__qmljs_increment; break;
                case IR::OpDecrement: op = VM::__qmljs_decrement; break;
                default: assert(!"unreachable"); break;
                } // switch

                if (op) {
                    Instruction::Unop unop;
                    unop.alu = op;
                    unop.e = e->index;
                    unop.targetTempIndex = targetTempIndex;
                    addInstruction(unop);
                } else {
                    qWarning("  UNOP1");
                }
            } else {
                qWarning("  UNOP2");
                s->dump(qout, IR::Stmt::MIR);
                qout << endl;
            }
        } else if (IR::Binop *b = s->source->asBinop()) {
            Instruction::Binop binop;
            binop.alu = aluOpFunction(b->op);
            binop.lhsIsTemp = toValueOrTemp(b->left, binop.lhs);
            binop.rhsIsTemp = toValueOrTemp(b->right, binop.rhs);
            binop.targetTempIndex = targetTempIndex;
            addInstruction(binop);
        } else if (IR::Call *c = s->source->asCall()) {
            if (c->base->asName()) {
                callActivationProperty(c, targetTempIndex);
            } else if (c->base->asMember()) {
                callProperty(c, targetTempIndex);
            } else if (c->base->asTemp()) {
                callValue(c, targetTempIndex);
            } else {
                Q_UNREACHABLE();
            }
        }
        return;
    } else if (IR::Name *n = s->target->asName()) {
        if (s->source->asTemp() || s->source->asConst()) {
            void (*op)(VM::Value value, VM::String *name, VM::ExecutionContext *ctx) = 0;
            switch (s->op) {
            case IR::OpBitAnd: op = VM::__qmljs_inplace_bit_and_name; break;
            case IR::OpBitOr: op = VM::__qmljs_inplace_bit_or_name; break;
            case IR::OpBitXor: op = VM::__qmljs_inplace_bit_xor_name; break;
            case IR::OpAdd: op = VM::__qmljs_inplace_add_name; break;
            case IR::OpSub: op = VM::__qmljs_inplace_sub_name; break;
            case IR::OpMul: op = VM::__qmljs_inplace_mul_name; break;
            case IR::OpDiv: op = VM::__qmljs_inplace_div_name; break;
            case IR::OpMod: op = VM::__qmljs_inplace_mod_name; break;
            case IR::OpLShift: op = VM::__qmljs_inplace_shl_name; break;
            case IR::OpRShift: op = VM::__qmljs_inplace_shr_name; break;
            case IR::OpURShift: op = VM::__qmljs_inplace_ushr_name; break;
            default: break;
            }

            if (op) {
                Instruction::InplaceNameOp ieo;
                ieo.alu = op;
                ieo.targetName = engine()->newString(*n->id);
                ieo.sourceIsTemp = toValueOrTemp(s->source, ieo.source);
                addInstruction(ieo);
                return;
            } else if (s->op == IR::OpInvalid) {
                Instruction::StoreName store;
                store.sourceIsTemp = toValueOrTemp(s->source, store.source);
                store.name = engine()->newString(*n->id);
                addInstruction(store);
                return;
            }
        }
        qWarning("NAME");
    } else if (IR::Subscript *ss = s->target->asSubscript()) {
        if (s->source->asTemp() || s->source->asConst()) {
            void (*op)(VM::Value base, VM::Value index, VM::Value value, VM::ExecutionContext *ctx) = 0;
            switch (s->op) {
            case IR::OpBitAnd: op = VM::__qmljs_inplace_bit_and_element; break;
            case IR::OpBitOr: op = VM::__qmljs_inplace_bit_or_element; break;
            case IR::OpBitXor: op = VM::__qmljs_inplace_bit_xor_element; break;
            case IR::OpAdd: op = VM::__qmljs_inplace_add_element; break;
            case IR::OpSub: op = VM::__qmljs_inplace_sub_element; break;
            case IR::OpMul: op = VM::__qmljs_inplace_mul_element; break;
            case IR::OpDiv: op = VM::__qmljs_inplace_div_element; break;
            case IR::OpMod: op = VM::__qmljs_inplace_mod_element; break;
            case IR::OpLShift: op = VM::__qmljs_inplace_shl_element; break;
            case IR::OpRShift: op = VM::__qmljs_inplace_shr_element; break;
            case IR::OpURShift: op = VM::__qmljs_inplace_ushr_element; break;
            default: break;
            }

            if (op) {
                Instruction::InplaceElementOp ieo;
                ieo.alu = op;
                ieo.targetBase = ss->base->asTemp()->index;
                ieo.targetIndex = ss->index->asTemp()->index;
                ieo.sourceIsTemp = toValueOrTemp(s->source, ieo.source);
                addInstruction(ieo);
                return;
            } else if (s->op == IR::OpInvalid) {
                Instruction::StoreElement store;
                store.base = ss->base->asTemp()->index;
                store.index = ss->index->asTemp()->index;
                store.sourceIsTemp = toValueOrTemp(s->source, store.source);
                addInstruction(store);
                return;
            }
        }
        qWarning("SUBSCRIPT");
    } else if (IR::Member *m = s->target->asMember()) {
        if (s->source->asTemp() || s->source->asConst()) {
            void (*op)(VM::Value value, VM::Value base, VM::String *name, VM::ExecutionContext *ctx) = 0;
            switch (s->op) {
            case IR::OpBitAnd: op = VM::__qmljs_inplace_bit_and_member; break;
            case IR::OpBitOr: op = VM::__qmljs_inplace_bit_or_member; break;
            case IR::OpBitXor: op = VM::__qmljs_inplace_bit_xor_member; break;
            case IR::OpAdd: op = VM::__qmljs_inplace_add_member; break;
            case IR::OpSub: op = VM::__qmljs_inplace_sub_member; break;
            case IR::OpMul: op = VM::__qmljs_inplace_mul_member; break;
            case IR::OpDiv: op = VM::__qmljs_inplace_div_member; break;
            case IR::OpMod: op = VM::__qmljs_inplace_mod_member; break;
            case IR::OpLShift: op = VM::__qmljs_inplace_shl_member; break;
            case IR::OpRShift: op = VM::__qmljs_inplace_shr_member; break;
            case IR::OpURShift: op = VM::__qmljs_inplace_ushr_member; break;
            default: break;
            }

            if (op) {
                Instruction::InplaceMemberOp imo;
                imo.alu = op;
                imo.targetBase = m->base->asTemp()->index;
                imo.targetMember = engine()->newString(*m->name);
                imo.sourceIsTemp = toValueOrTemp(s->source, imo.source);
                addInstruction(imo);
                return;
            } else if (s->op == IR::OpInvalid) {
                Instruction::StoreProperty store;
                store.baseTemp = m->base->asTemp()->index;
                store.name = engine()->newString(*m->name);
                store.sourceIsTemp = toValueOrTemp(s->source, store.source);
                addInstruction(store);
                return;
            }
        }
        qWarning("MEMBER");
    }

    Q_UNIMPLEMENTED();
    s->dump(qout, IR::Stmt::MIR);
    qout << endl;
    Q_UNREACHABLE();
}

void InstructionSelection::visitJump(IR::Jump *s)
{
    Instruction::Jump jump;
    jump.offset = 0;
    ptrdiff_t loc = addInstruction(jump) + (((const char *)&jump.offset) - ((const char *)&jump));

    _patches[s->target].append(loc);
}

void InstructionSelection::visitCJump(IR::CJump *s)
{
    int tempIndex;
    if (IR::Temp *t = s->cond->asTemp()) {
        tempIndex = t->index;
    } else if (IR::Binop *b = s->cond->asBinop()) {
        tempIndex = scratchTempIndex();
        Instruction::Binop binop;
        binop.alu = aluOpFunction(b->op);
        binop.lhsIsTemp = toValueOrTemp(b->left, binop.lhs);
        binop.rhsIsTemp = toValueOrTemp(b->right, binop.rhs);
        binop.targetTempIndex = tempIndex;
        addInstruction(binop);
    } else {
        Q_UNREACHABLE();
    }

    Instruction::CJump jump;
    jump.offset = 0;
    jump.tempIndex = tempIndex;
    ptrdiff_t tl = addInstruction(jump) + (((const char *)&jump.offset) - ((const char *)&jump));
    _patches[s->iftrue].append(tl);

    if (_block->index + 1 != s->iffalse->index) {
        Instruction::Jump jump;
        jump.offset = 0;
        ptrdiff_t fl = addInstruction(jump) + (((const char *)&jump.offset) - ((const char *)&jump));
        _patches[s->iffalse].append(fl);
    }
}

void InstructionSelection::visitRet(IR::Ret *s)
{
    Instruction::Ret ret;
    ret.tempIndex = s->expr->index;
    addInstruction(ret);
}

ptrdiff_t InstructionSelection::addInstructionHelper(Instr::Type type, Instr &instr)
{
#ifdef MOTH_THREADED_INTERPRETER
    instr.common.code = VME::instructionJumpTable()[static_cast<int>(type)];
#else
    instr.common.instructionType = type;
#endif

    ptrdiff_t ptrOffset = _ccode - _code;
    int size = Instr::size(type);

    ::memcpy(_ccode, reinterpret_cast<const char *>(&instr), size);
    _ccode += size;

    return ptrOffset;
}

