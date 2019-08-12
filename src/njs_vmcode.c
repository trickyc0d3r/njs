
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */


#include <njs_main.h>


struct njs_property_next_s {
    uint32_t     index;
    njs_array_t  *array;
};

static njs_jump_off_t njs_vmcode_object(njs_vm_t *vm);
static njs_jump_off_t njs_vmcode_array(njs_vm_t *vm, u_char *pc);
static njs_jump_off_t njs_vmcode_function(njs_vm_t *vm, u_char *pc);
static njs_jump_off_t njs_vmcode_arguments(njs_vm_t *vm, u_char *pc);
static njs_jump_off_t njs_vmcode_regexp(njs_vm_t *vm, u_char *pc);
static njs_jump_off_t njs_vmcode_template_literal(njs_vm_t *vm,
    njs_value_t *inlvd1, njs_value_t *inlvd2);
static njs_jump_off_t njs_vmcode_object_copy(njs_vm_t *vm, njs_value_t *value,
    njs_value_t *invld);

static njs_jump_off_t njs_vmcode_property_init(njs_vm_t *vm,
    njs_value_t *value, njs_value_t *key, njs_value_t *retval);
static njs_jump_off_t njs_vmcode_property_in(njs_vm_t *vm,
    njs_value_t *value, njs_value_t *key);
static njs_jump_off_t njs_vmcode_property_delete(njs_vm_t *vm,
    njs_value_t *value, njs_value_t *key);
static njs_jump_off_t njs_vmcode_property_foreach(njs_vm_t *vm,
    njs_value_t *object, njs_value_t *invld, u_char *pc);
static njs_jump_off_t njs_vmcode_property_next(njs_vm_t *vm,
    njs_value_t *object, njs_value_t *value, u_char *pc);
static njs_jump_off_t njs_vmcode_instance_of(njs_vm_t *vm, njs_value_t *object,
    njs_value_t *constructor);
static njs_jump_off_t njs_vmcode_typeof(njs_vm_t *vm, njs_value_t *value,
    njs_value_t *invld);

static njs_jump_off_t njs_vmcode_return(njs_vm_t *vm, njs_value_t *invld,
    njs_value_t *retval);

static njs_jump_off_t njs_vmcode_try_start(njs_vm_t *vm, njs_value_t *value,
    njs_value_t *offset, u_char *pc);
static njs_jump_off_t njs_vmcode_try_break(njs_vm_t *vm, njs_value_t *value,
    njs_value_t *offset);
static njs_jump_off_t njs_vmcode_try_continue(njs_vm_t *vm, njs_value_t *value,
    njs_value_t *offset);
static njs_jump_off_t njs_vmcode_try_end(njs_vm_t *vm, njs_value_t *invld,
    njs_value_t *offset);
static njs_jump_off_t njs_vmcode_finally(njs_vm_t *vm, njs_value_t *invld,
    njs_value_t *retval, u_char *pc);
static void njs_vmcode_reference_error(njs_vm_t *vm, u_char *pc);

/*
 * These functions are forbidden to inline to minimize JavaScript VM
 * interpreter memory footprint.  The size is less than 8K on AMD64
 * and should fit in CPU L1 instruction cache.
 */

static njs_jump_off_t njs_string_concat(njs_vm_t *vm, njs_value_t *val1,
    njs_value_t *val2);
static njs_jump_off_t njs_values_equal(njs_vm_t *vm, njs_value_t *val1,
    njs_value_t *val2);
static njs_jump_off_t njs_primitive_values_compare(njs_vm_t *vm,
    njs_value_t *val1, njs_value_t *val2);
static njs_jump_off_t njs_function_frame_create(njs_vm_t *vm,
    njs_value_t *value, const njs_value_t *this, uintptr_t nargs,
    njs_bool_t ctor);
static njs_object_t *njs_function_new_object(njs_vm_t *vm, njs_value_t *value);

/*
 * The nJSVM is optimized for an ABIs where the first several arguments
 * are passed in registers (AMD64, ARM32/64): two pointers to the operand
 * values is passed as arguments although they are not always used.
 */

njs_int_t
njs_vmcode_interpreter(njs_vm_t *vm, u_char *pc)
{
    u_char                       *catch;
    double                       num, exponent;
    int32_t                      i32;
    uint32_t                     u32;
    njs_str_t                    string;
    njs_uint_t                   hint;
    njs_bool_t                   valid, lambda_call;
    njs_value_t                  *retval, *value1, *value2, *src, *s1, *s2;
    njs_value_t                  numeric1, numeric2, primitive1, primitive2,
                                 dst;
    njs_frame_t                  *frame;
    njs_jump_off_t               ret;
    njs_vmcode_this_t            *this;
    njs_native_frame_t           *previous;
    njs_property_next_t          *next;
    njs_vmcode_generic_t         *vmcode;
    njs_vmcode_prop_get_t        *get;
    njs_vmcode_prop_set_t        *set;
    njs_vmcode_operation_t       op;
    njs_vmcode_prop_next_t       *pnext;
    njs_vmcode_test_jump_t       *test_jump;
    njs_vmcode_equal_jump_t      *equal;
    njs_vmcode_try_return_t      *try_return;
    njs_vmcode_method_frame_t    *method_frame;
    njs_vmcode_function_frame_t  *function_frame;

next:

    for ( ;; ) {

        vmcode = (njs_vmcode_generic_t *) pc;

        /*
         * The first operand is passed as is in value2 to
         *   NJS_VMCODE_JUMP,
         *   NJS_VMCODE_IF_TRUE_JUMP,
         *   NJS_VMCODE_IF_FALSE_JUMP,
         *   NJS_VMCODE_FUNCTION_FRAME,
         *   NJS_VMCODE_FUNCTION_CALL,
         *   NJS_VMCODE_RETURN,
         *   NJS_VMCODE_TRY_START,
         *   NJS_VMCODE_TRY_CONTINUE,
         *   NJS_VMCODE_TRY_BREAK,
         *   NJS_VMCODE_TRY_END,
         *   NJS_VMCODE_CATCH,
         *   NJS_VMCODE_THROW,
         *   NJS_VMCODE_STOP.
         */
        value2 = (njs_value_t *) vmcode->operand1;
        value1 = NULL;

        switch (vmcode->code.operands) {

        case NJS_VMCODE_3OPERANDS:
            value2 = njs_vmcode_operand(vm, vmcode->operand3);

            /* Fall through. */

        case NJS_VMCODE_2OPERANDS:
            value1 = njs_vmcode_operand(vm, vmcode->operand2);
        }

        op = vmcode->code.operation;

        /*
         * On success an operation returns size of the bytecode,
         * a jump offset or zero after the call or return operations.
         * Jumps can return a negative offset.  Compilers can generate
         *    (ret < 0 && ret >= NJS_PREEMPT)
         * as a single unsigned comparision.
         */

        if (op > NJS_VMCODE_NORET) {

            if (op == NJS_VMCODE_MOVE) {
                retval = njs_vmcode_operand(vm, vmcode->operand1);
                *retval = *value1;

                pc += sizeof(njs_vmcode_move_t);
                goto next;
            }

            if (op == NJS_VMCODE_PROPERTY_GET) {
                get = (njs_vmcode_prop_get_t *) pc;
                retval = njs_vmcode_operand(vm, get->value);

                ret = njs_value_property(vm, value1, value2, retval);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto error;
                }

                pc += sizeof(njs_vmcode_prop_get_t);
                goto next;
            }

            switch (op) {
            case NJS_VMCODE_INCREMENT:
            case NJS_VMCODE_POST_INCREMENT:
            case NJS_VMCODE_DECREMENT:
            case NJS_VMCODE_POST_DECREMENT:
                if (njs_slow_path(!njs_is_numeric(value2))) {
                    ret = njs_value_to_numeric(vm, &numeric1, value2);
                    if (njs_slow_path(ret != NJS_OK)) {
                        goto error;
                    }

                    num = njs_number(&numeric1);

                } else {
                    num = njs_number(value2);
                }

                njs_set_number(value1,
                           num + (1 - 2 * ((op - NJS_VMCODE_INCREMENT) >> 1)));

                retval = njs_vmcode_operand(vm, vmcode->operand1);

                if (op & 1) {
                    njs_set_number(retval, num);

                } else {
                    *retval = *value1;
                }

                pc += sizeof(njs_vmcode_3addr_t);
                goto next;

            /*
             * njs_vmcode_try_return() saves a return value to use it later by
             * njs_vmcode_finally(), and jumps to the nearest try_break block.
             */
            case NJS_VMCODE_TRY_RETURN:
                retval = njs_vmcode_operand(vm, vmcode->operand1);
                *retval = *value1;

                try_return = (njs_vmcode_try_return_t *) pc;
                pc += try_return->offset;
                goto next;

            case NJS_VMCODE_LESS:
            case NJS_VMCODE_GREATER:
            case NJS_VMCODE_LESS_OR_EQUAL:
            case NJS_VMCODE_GREATER_OR_EQUAL:
            case NJS_VMCODE_ADDITION:
                if (njs_slow_path(!njs_is_primitive(value1))) {
                    hint = (op == NJS_VMCODE_ADDITION) && njs_is_date(value1);
                    ret = njs_value_to_primitive(vm, &primitive1, value1, hint);
                    if (ret != NJS_OK) {
                        goto error;
                    }

                    value1 = &primitive1;
                }

                if (njs_slow_path(!njs_is_primitive(value2))) {
                    hint = (op == NJS_VMCODE_ADDITION) && njs_is_date(value2);
                    ret = njs_value_to_primitive(vm, &primitive2, value2, hint);
                    if (ret != NJS_OK) {
                        goto error;
                    }

                    value2 = &primitive2;
                }

                retval = njs_vmcode_operand(vm, vmcode->operand1);

                if (op == NJS_VMCODE_ADDITION) {
                    if (njs_fast_path(njs_is_numeric(value1)
                                      && njs_is_numeric(value2)))
                    {
                        njs_set_number(retval, njs_number(value1)
                                               + njs_number(value2));
                        pc += sizeof(njs_vmcode_3addr_t);
                        goto next;
                    }

                    if (njs_is_string(value1)) {
                        s1 = value1;
                        s2 = &dst;
                        src = value2;

                    } else {
                        s1 = &dst;
                        s2 = value2;
                        src = value1;
                    }

                    ret = njs_primitive_value_to_string(vm, &dst, src);
                    if (njs_slow_path(ret != NJS_OK)) {
                        goto error;
                    }

                    ret = njs_string_concat(vm, s1, s2);
                    if (njs_slow_path(ret == NJS_ERROR)) {
                        goto error;
                    }

                    *retval = vm->retval;

                    pc += ret;
                    goto next;
                }

                if ((uint8_t) (op - NJS_VMCODE_GREATER) < 2) {
                    /* NJS_VMCODE_GREATER, NJS_VMCODE_LESS_OR_EQUAL */
                    src = value1;
                    value1 = value2;
                    value2 = src;
                }

                ret = njs_primitive_values_compare(vm, value1, value2);

                if (op < NJS_VMCODE_LESS_OR_EQUAL) {
                    ret = ret > 0;

                } else {
                    ret = ret == 0;
                }

                njs_set_boolean(retval, ret);

                pc += sizeof(njs_vmcode_3addr_t);
                goto next;

            case NJS_VMCODE_EQUAL:
            case NJS_VMCODE_NOT_EQUAL:
                ret = njs_values_equal(vm, value1, value2);
                if (njs_slow_path(ret < 0)) {
                    goto error;
                }

                ret ^= op - NJS_VMCODE_EQUAL;

                retval = njs_vmcode_operand(vm, vmcode->operand1);
                njs_set_boolean(retval, ret);

                pc += sizeof(njs_vmcode_3addr_t);
                goto next;

            case NJS_VMCODE_SUBSTRACTION:
            case NJS_VMCODE_MULTIPLICATION:
            case NJS_VMCODE_EXPONENTIATION:
            case NJS_VMCODE_DIVISION:
            case NJS_VMCODE_REMAINDER:
            case NJS_VMCODE_BITWISE_AND:
            case NJS_VMCODE_BITWISE_OR:
            case NJS_VMCODE_BITWISE_XOR:
            case NJS_VMCODE_LEFT_SHIFT:
            case NJS_VMCODE_RIGHT_SHIFT:
            case NJS_VMCODE_UNSIGNED_RIGHT_SHIFT:
                if (njs_slow_path(!njs_is_numeric(value1))) {
                    ret = njs_value_to_numeric(vm, &numeric1, value1);
                    if (ret != NJS_OK) {
                        goto error;
                    }

                    value1 = &numeric1;
                }

                if (njs_slow_path(!njs_is_numeric(value2))) {
                    ret = njs_value_to_numeric(vm, &numeric2, value2);
                    if (ret != NJS_OK) {
                        goto error;
                    }

                    value2 = &numeric2;
                }

                num = njs_number(value1);

                retval = njs_vmcode_operand(vm, vmcode->operand1);
                pc += sizeof(njs_vmcode_3addr_t);

                switch (op) {
                case NJS_VMCODE_SUBSTRACTION:
                    num -= njs_number(value2);
                    break;

                case NJS_VMCODE_MULTIPLICATION:
                    num *= njs_number(value2);
                    break;

                case NJS_VMCODE_EXPONENTIATION:
                    exponent = njs_number(value2);

                    /*
                     * According to ES7:
                     *  1. If exponent is NaN, the result should be NaN;
                     *  2. The result of +/-1 ** +/-Infinity should be NaN.
                     */
                    valid = njs_expect(1, fabs(num) != 1
                                          || (!isnan(exponent)
                                              && !isinf(exponent)));

                    num = valid ? pow(num, exponent) : NAN;
                    break;

                case NJS_VMCODE_DIVISION:
                    num /= njs_number(value2);
                    break;

                case NJS_VMCODE_REMAINDER:
                    num = fmod(num, njs_number(value2));
                    break;

                case NJS_VMCODE_BITWISE_AND:
                case NJS_VMCODE_BITWISE_OR:
                case NJS_VMCODE_BITWISE_XOR:
                    i32 = njs_number_to_int32(njs_number(value2));

                    switch (op) {
                    case NJS_VMCODE_BITWISE_AND:
                        i32 &= njs_number_to_int32(num);
                        break;

                    case NJS_VMCODE_BITWISE_OR:
                        i32 |= njs_number_to_int32(num);
                        break;

                    case NJS_VMCODE_BITWISE_XOR:
                        i32 ^= njs_number_to_int32(num);
                        break;
                    }

                    njs_set_int32(retval, i32);
                    goto next;

                default:
                    u32 = njs_number_to_uint32(njs_number(value2)) & 0x1f;

                    switch (op) {
                    case NJS_VMCODE_LEFT_SHIFT:
                    case NJS_VMCODE_RIGHT_SHIFT:
                        i32 = njs_number_to_int32(num);

                        if (op == NJS_VMCODE_LEFT_SHIFT) {
                            /* Shifting of negative numbers is undefined. */
                            i32 = (uint32_t) i32 << u32;
                        } else {
                            i32 >>= u32;
                        }

                        njs_set_int32(retval, i32);
                        break;

                    default: /* NJS_VMCODE_UNSIGNED_RIGHT_SHIFT */
                        njs_set_uint32(retval,
                                       njs_number_to_uint32(num) >> u32);
                    }

                    goto next;
                }

                njs_set_number(retval, num);
                goto next;

            case NJS_VMCODE_OBJECT_COPY:
                ret = njs_vmcode_object_copy(vm, value1, value2);
                break;

            case NJS_VMCODE_TEMPLATE_LITERAL:
                ret = njs_vmcode_template_literal(vm, value1, value2);
                break;

            case NJS_VMCODE_PROPERTY_IN:
                ret = njs_vmcode_property_in(vm, value1, value2);
                break;

            case NJS_VMCODE_PROPERTY_DELETE:
                ret = njs_vmcode_property_delete(vm, value1, value2);
                break;

            case NJS_VMCODE_PROPERTY_FOREACH:
                ret = njs_vmcode_property_foreach(vm, value1, value2, pc);
                break;

            case NJS_VMCODE_STRICT_EQUAL:
            case NJS_VMCODE_STRICT_NOT_EQUAL:
                ret = njs_values_strict_equal(value1, value2);

                ret ^= op - NJS_VMCODE_STRICT_EQUAL;

                retval = njs_vmcode_operand(vm, vmcode->operand1);
                njs_set_boolean(retval, ret);

                pc += sizeof(njs_vmcode_3addr_t);
                goto next;

            case NJS_VMCODE_TEST_IF_TRUE:
            case NJS_VMCODE_TEST_IF_FALSE:
                ret = njs_is_true(value1);

                ret ^= op - NJS_VMCODE_TEST_IF_TRUE;

                if (ret) {
                    test_jump = (njs_vmcode_test_jump_t *) pc;
                    ret = test_jump->offset;

                } else {
                    ret = sizeof(njs_vmcode_3addr_t);
                }

                retval = njs_vmcode_operand(vm, vmcode->operand1);
                *retval = *value1;

                pc += ret;
                goto next;

            case NJS_VMCODE_UNARY_PLUS:
            case NJS_VMCODE_UNARY_NEGATION:
            case NJS_VMCODE_BITWISE_NOT:
                if (njs_slow_path(!njs_is_numeric(value1))) {
                    ret = njs_value_to_numeric(vm, &numeric1, value1);
                    if (ret != NJS_OK) {
                        goto error;
                    }

                    value1 = &numeric1;
                }

                num = njs_number(value1);
                retval = njs_vmcode_operand(vm, vmcode->operand1);

                switch (op) {
                case NJS_VMCODE_UNARY_NEGATION:
                    num = -num;

                    /* Fall through. */
                case NJS_VMCODE_UNARY_PLUS:
                    njs_set_number(retval, num);
                    break;

                case NJS_VMCODE_BITWISE_NOT:
                    njs_set_int32(retval, ~njs_number_to_integer(num));
                }

                pc += sizeof(njs_vmcode_2addr_t);
                goto next;

            case NJS_VMCODE_LOGICAL_NOT:
                retval = njs_vmcode_operand(vm, vmcode->operand1);
                njs_set_boolean(retval, !njs_is_true(value1));

                pc += sizeof(njs_vmcode_2addr_t);
                goto next;

            case NJS_VMCODE_OBJECT:
                ret = njs_vmcode_object(vm);
                break;

            case NJS_VMCODE_ARRAY:
                ret = njs_vmcode_array(vm, pc);
                break;

            case NJS_VMCODE_FUNCTION:
                ret = njs_vmcode_function(vm, pc);
                break;

            case NJS_VMCODE_REGEXP:
                ret = njs_vmcode_regexp(vm, pc);
                break;

            case NJS_VMCODE_INSTANCE_OF:
                ret = njs_vmcode_instance_of(vm, value1, value2);
                break;

            case NJS_VMCODE_TYPEOF:
                ret = njs_vmcode_typeof(vm, value1, value2);
                break;

            case NJS_VMCODE_VOID:
                vm->retval = njs_value_undefined;

                ret = sizeof(njs_vmcode_2addr_t);
                break;

            case NJS_VMCODE_DELETE:
                njs_release(vm, value1);
                vm->retval = njs_value_true;

                ret = sizeof(njs_vmcode_2addr_t);
                break;

            default:
                njs_internal_error(vm, "%d has retval", op);
                goto error;
            }

            if (njs_slow_path(ret < 0 && ret >= NJS_PREEMPT)) {
                break;
            }

            retval = njs_vmcode_operand(vm, vmcode->operand1);
            njs_release(vm, retval);
            *retval = vm->retval;

        } else {
            switch (op) {
            case NJS_VMCODE_STOP:
                value2 = njs_vmcode_operand(vm, value2);
                vm->retval = *value2;

                return NJS_OK;

            case NJS_VMCODE_JUMP:
                ret = (njs_jump_off_t) value2;
                break;

            case NJS_VMCODE_PROPERTY_SET:
                set = (njs_vmcode_prop_set_t *) pc;
                retval = njs_vmcode_operand(vm, set->value);

                ret = njs_value_property_set(vm, value1, value2, retval);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto error;
                }

                ret = sizeof(njs_vmcode_prop_set_t);
                break;

            case NJS_VMCODE_IF_TRUE_JUMP:
            case NJS_VMCODE_IF_FALSE_JUMP:
                ret = njs_is_true(value1);

                ret ^= op - NJS_VMCODE_IF_TRUE_JUMP;

                ret = ret ? (njs_jump_off_t) value2
                          : (njs_jump_off_t) sizeof(njs_vmcode_cond_jump_t);

                break;

            case NJS_VMCODE_IF_EQUAL_JUMP:
                if (njs_values_strict_equal(value1, value2)) {
                    equal = (njs_vmcode_equal_jump_t *) pc;
                    ret = equal->offset;

                } else {
                    ret = sizeof(njs_vmcode_3addr_t);
                }

                break;

            case NJS_VMCODE_PROPERTY_INIT:
                set = (njs_vmcode_prop_set_t *) pc;
                retval = njs_vmcode_operand(vm, set->value);
                ret = njs_vmcode_property_init(vm, value1, value2, retval);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto error;
                }

                break;

            case NJS_VMCODE_RETURN:
                value2 = njs_vmcode_operand(vm, value2);

                frame = (njs_frame_t *) vm->top_frame;

                if (frame->native.ctor) {
                    if (njs_is_object(value2)) {
                        njs_release(vm, vm->scopes[NJS_SCOPE_ARGUMENTS]);

                    } else {
                        value2 = vm->scopes[NJS_SCOPE_ARGUMENTS];
                    }
                }

                previous = njs_function_previous_frame(&frame->native);

                njs_vm_scopes_restore(vm, frame, previous);

                /*
                 * If a retval is in a callee arguments scope it
                 * must be in the previous callee arguments scope.
                 */
                retval = njs_vmcode_operand(vm, frame->retval);

                /*
                 * GC: value external/internal++ depending on
                 * value and retval type
                 */
                *retval = *value2;

                njs_function_frame_free(vm, &frame->native);

                return NJS_OK;

            case NJS_VMCODE_FUNCTION_FRAME:
                function_frame = (njs_vmcode_function_frame_t *) pc;

                /* TODO: external object instead of void this. */

                ret = njs_function_frame_create(vm, value1,
                                                &njs_value_undefined,
                                                (uintptr_t) value2,
                                                function_frame->ctor);

                if (njs_slow_path(ret != NJS_OK)) {
                    goto error;
                }

                ret = sizeof(njs_vmcode_function_frame_t);
                break;

            case NJS_VMCODE_METHOD_FRAME:
                method_frame = (njs_vmcode_method_frame_t *) pc;

                ret = njs_value_property(vm, value1, value2, &dst);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto error;
                }

                if (njs_slow_path(!njs_is_function(&dst))) {
                    ret = njs_value_to_string(vm, value2, value2);
                    if (njs_slow_path(ret != NJS_OK)) {
                        return NJS_ERROR;
                    }

                    njs_string_get(value2, &string);
                    njs_type_error(vm,
                               "(intermediate value)[\"%V\"] is not a function",
                               &string);
                    goto error;
                }

                ret = njs_function_frame_create(vm, &dst, value1,
                                                method_frame->nargs,
                                                method_frame->ctor);

                if (njs_slow_path(ret != NJS_OK)) {
                    goto error;
                }

                ret = sizeof(njs_vmcode_method_frame_t);
                break;

            case NJS_VMCODE_FUNCTION_CALL:
                ret = njs_function_frame_invoke(vm, (njs_index_t) value2);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto error;
                }

                ret = sizeof(njs_vmcode_function_call_t);
                break;

            case NJS_VMCODE_PROPERTY_NEXT:
                if (!njs_is_external(value1)) {
                    pnext = (njs_vmcode_prop_next_t *) pc;
                    retval = njs_vmcode_operand(vm, pnext->retval);

                    next = value2->data.u.next;

                    if (next->index < next->array->length) {
                        *retval = next->array->data[next->index++];

                        ret = pnext->offset;
                        break;
                    }
                }

                ret = njs_vmcode_property_next(vm, value1, value2, pc);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto error;
                }

                break;

            case NJS_VMCODE_THIS:
                frame = vm->active_frame;
                this = (njs_vmcode_this_t *) pc;

                retval = njs_vmcode_operand(vm, this->dst);
                *retval = frame->native.arguments[0];

                ret = sizeof(njs_vmcode_this_t);
                break;

            case NJS_VMCODE_ARGUMENTS:
                ret = njs_vmcode_arguments(vm, pc);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto error;
                }

                break;

            case NJS_VMCODE_TRY_START:
                ret = njs_vmcode_try_start(vm, value1, value2, pc);
                if (njs_slow_path(ret == NJS_ERROR)) {
                    goto error;
                }

                break;

            case NJS_VMCODE_THROW:
                value2 = njs_vmcode_operand(vm, value2);
                vm->retval = *value2;
                goto error;

            case NJS_VMCODE_TRY_BREAK:
                ret = njs_vmcode_try_break(vm, value1, value2);
                break;

            case NJS_VMCODE_TRY_CONTINUE:
                ret = njs_vmcode_try_continue(vm, value1, value2);
                break;

            case NJS_VMCODE_TRY_END:
                ret = njs_vmcode_try_end(vm, value1, value2);
                break;

            /*
             * njs_vmcode_catch() is set on the start of a "catch" block to
             * store exception and to remove a "try" block if there is no
             * "finally" block or to update a catch address to the start of
             * a "finally" block.
             * njs_vmcode_catch() is set on the start of a "finally" block
             * to store uncaught exception and to remove a "try" block.
             */
            case NJS_VMCODE_CATCH:
                *value1 = vm->retval;

                if ((njs_jump_off_t) value2 == sizeof(njs_vmcode_catch_t)) {
                    ret = njs_vmcode_try_end(vm, value1, value2);

                } else {
                    vm->top_frame->exception.catch =
                                                  pc + (njs_jump_off_t) value2;
                    ret = sizeof(njs_vmcode_catch_t);
                }

                break;

            case NJS_VMCODE_FINALLY:
                ret = njs_vmcode_finally(vm, value1, value2, pc);

                switch (ret) {
                case NJS_OK:
                    return NJS_OK;
                case NJS_ERROR:
                    goto error;
                }

                break;

            case NJS_VMCODE_REFERENCE_ERROR:
                njs_vmcode_reference_error(vm, pc);
                goto error;

            default:
                njs_internal_error(vm, "%d has NO retval", op);
                goto error;
            }
        }

        pc += ret;
    }

error:

    for ( ;; ) {
        frame = (njs_frame_t *) vm->top_frame;

        catch = frame->native.exception.catch;

        if (catch != NULL) {
            pc = catch;

            if (vm->debug != NULL) {
                njs_arr_reset(vm->backtrace);
            }

            goto next;
        }

        if (vm->debug != NULL
            && njs_vm_add_backtrace_entry(vm, frame) != NJS_OK)
        {
            break;
        }

        previous = frame->native.previous;
        if (previous == NULL) {
            break;
        }

        lambda_call = (frame == vm->active_frame);

        njs_vm_scopes_restore(vm, frame, previous);

        if (frame->native.size != 0) {
            vm->stack_size -= frame->native.size;
            njs_mp_free(vm->mem_pool, frame);
        }

        if (lambda_call) {
            break;
        }
    }

    return NJS_ERROR;
}


static njs_jump_off_t
njs_vmcode_object(njs_vm_t *vm)
{
    njs_object_t  *object;

    object = njs_object_alloc(vm);

    if (njs_fast_path(object != NULL)) {
        njs_set_object(&vm->retval, object);

        return sizeof(njs_vmcode_object_t);
    }

    return NJS_ERROR;
}


static njs_jump_off_t
njs_vmcode_array(njs_vm_t *vm, u_char *pc)
{
    uint32_t            length;
    njs_array_t         *array;
    njs_value_t         *value;
    njs_vmcode_array_t  *code;

    code = (njs_vmcode_array_t *) pc;

    array = njs_array_alloc(vm, code->length, NJS_ARRAY_SPARE);

    if (njs_fast_path(array != NULL)) {

        if (code->ctor) {
            /* Array of the form [,,,], [1,,]. */
            value = array->start;
            length = array->length;

            do {
                njs_set_invalid(value);
                value++;
                length--;
            } while (length != 0);

        } else {
            /* Array of the form [], [,,1], [1,2,3]. */
            array->length = 0;
        }

        njs_set_array(&vm->retval, array);

        return sizeof(njs_vmcode_array_t);
    }

    return NJS_ERROR;
}


static njs_jump_off_t
njs_vmcode_function(njs_vm_t *vm, u_char *pc)
{
    njs_function_t         *function;
    njs_function_lambda_t  *lambda;
    njs_vmcode_function_t  *code;

    code = (njs_vmcode_function_t *) pc;
    lambda = code->lambda;

    function = njs_function_alloc(vm, lambda, vm->active_frame->closures, 0);
    if (njs_slow_path(function == NULL)) {
        return NJS_ERROR;
    }

    njs_set_function(&vm->retval, function);

    return sizeof(njs_vmcode_function_t);
}


static njs_jump_off_t
njs_vmcode_arguments(njs_vm_t *vm, u_char *pc)
{
    njs_frame_t             *frame;
    njs_value_t             *value;
    njs_jump_off_t          ret;
    njs_vmcode_arguments_t  *code;

    frame = (njs_frame_t *) vm->active_frame;

    if (frame->native.arguments_object == NULL) {
        ret = njs_function_arguments_object_init(vm, &frame->native);
        if (njs_slow_path(ret != NJS_OK)) {
            return NJS_ERROR;
        }
    }

    code = (njs_vmcode_arguments_t *) pc;

    value = njs_vmcode_operand(vm, code->dst);
    njs_set_object(value, frame->native.arguments_object);

    return sizeof(njs_vmcode_arguments_t);
}


static njs_jump_off_t
njs_vmcode_regexp(njs_vm_t *vm, u_char *pc)
{
    njs_regexp_t         *regexp;
    njs_vmcode_regexp_t  *code;

    code = (njs_vmcode_regexp_t *) pc;

    regexp = njs_regexp_alloc(vm, code->pattern);

    if (njs_fast_path(regexp != NULL)) {
        njs_set_regexp(&vm->retval, regexp);

        return sizeof(njs_vmcode_regexp_t);
    }

    return NJS_ERROR;
}


static njs_jump_off_t
njs_vmcode_template_literal(njs_vm_t *vm, njs_value_t *invld1,
    njs_value_t *retval)
{
    njs_array_t     *array;
    njs_value_t     *value;
    njs_jump_off_t  ret;

    static const njs_function_t  concat = {
          .native = 1,
          .args_offset = 1,
          .u.native = njs_string_prototype_concat
    };

    value = njs_vmcode_operand(vm, retval);

    if (!njs_is_primitive(value)) {
        array = njs_array(value);

        ret = njs_function_frame(vm, (njs_function_t *) &concat,
                                 &njs_string_empty, array->start,
                                 array->length, 0);
        if (njs_slow_path(ret != NJS_OK)) {
            return ret;
        }

        ret = njs_function_frame_invoke(vm, (njs_index_t) retval);
        if (njs_slow_path(ret != NJS_OK)) {
            return ret;
        }
    }

    return sizeof(njs_vmcode_template_literal_t);
}


static njs_jump_off_t
njs_vmcode_object_copy(njs_vm_t *vm, njs_value_t *value, njs_value_t *invld)
{
    njs_object_t    *object;
    njs_function_t  *function;

    switch (value->type) {

    case NJS_OBJECT:
        object = njs_object_value_copy(vm, value);
        if (njs_slow_path(object == NULL)) {
            return NJS_ERROR;
        }

        break;

    case NJS_FUNCTION:
        function = njs_function_value_copy(vm, value);
        if (njs_slow_path(function == NULL)) {
            return NJS_ERROR;
        }

        break;

    default:
        break;
    }

    vm->retval = *value;

    njs_retain(value);

    return sizeof(njs_vmcode_object_copy_t);
}


static njs_jump_off_t
njs_vmcode_property_init(njs_vm_t *vm, njs_value_t *value, njs_value_t *key,
    njs_value_t *init)
{
    uint32_t            index, size;
    njs_array_t         *array;
    njs_value_t         *val, name;
    njs_object_t        *obj;
    njs_jump_off_t      ret;
    njs_object_prop_t   *prop;
    njs_lvlhsh_query_t  lhq;

    switch (value->type) {
    case NJS_ARRAY:
        index = njs_value_to_index(key);
        if (njs_slow_path(index == NJS_ARRAY_INVALID_INDEX)) {
            njs_internal_error(vm,
                               "invalid index while property initialization");
            return NJS_ERROR;
        }

        array = value->data.u.array;

        if (index >= array->length) {
            size = index - array->length;

            ret = njs_array_expand(vm, array, 0, size + 1);
            if (njs_slow_path(ret != NJS_OK)) {
                return ret;
            }

            val = &array->start[array->length];

            while (size != 0) {
                njs_set_invalid(val);
                val++;
                size--;
            }

            array->length = index + 1;
        }

        /* GC: retain. */
        array->start[index] = *init;

        break;

    case NJS_OBJECT:
        ret = njs_value_to_string(vm, &name, key);
        if (njs_slow_path(ret != NJS_OK)) {
            return NJS_ERROR;
        }

        njs_string_get(&name, &lhq.key);
        lhq.key_hash = njs_djb_hash(lhq.key.start, lhq.key.length);
        lhq.proto = &njs_object_hash_proto;
        lhq.pool = vm->mem_pool;

        obj = njs_object(value);

        if (obj->__proto__ != NULL) {
            /* obj->__proto__ can be NULL after __proto__: null assignment */
            ret = njs_lvlhsh_find(&obj->__proto__->shared_hash, &lhq);
            if (ret == NJS_OK) {
                prop = lhq.value;

                if (prop->type == NJS_PROPERTY_HANDLER) {
                    ret = prop->value.data.u.prop_handler(vm, value, init,
                                                          &vm->retval);
                    if (njs_slow_path(ret != NJS_OK)) {
                        return ret;
                    }

                    break;
                }
            }
        }

        prop = njs_object_prop_alloc(vm, &name, init, 1);
        if (njs_slow_path(prop == NULL)) {
            return NJS_ERROR;
        }

        lhq.value = prop;
        lhq.replace = 1;

        ret = njs_lvlhsh_insert(&obj->hash, &lhq);
        if (njs_slow_path(ret != NJS_OK)) {
            njs_internal_error(vm, "lvlhsh insert/replace failed");
            return NJS_ERROR;
        }

        break;

    default:
        njs_internal_error(vm, "unexpected object type \"%s\" "
                           "while property initialization",
                           njs_type_string(value->type));

        return NJS_ERROR;
    }

    return sizeof(njs_vmcode_prop_set_t);
}


static njs_jump_off_t
njs_vmcode_property_in(njs_vm_t *vm, njs_value_t *value, njs_value_t *key)
{
    njs_int_t             ret;
    njs_bool_t            found;
    njs_object_prop_t     *prop;
    njs_property_query_t  pq;

    found = 0;

    njs_property_query_init(&pq, NJS_PROPERTY_QUERY_GET, 0);

    ret = njs_property_query(vm, &pq, value, key);
    if (njs_slow_path(ret == NJS_ERROR)) {
        return ret;
    }

    if (ret == NJS_DECLINED) {
        if (!njs_is_object(value) && !njs_is_external(value)) {
            njs_type_error(vm, "property in on a primitive value");

            return NJS_ERROR;
        }

    } else {
        prop = pq.lhq.value;

        if (/* !njs_is_data_descriptor(prop) */
            prop->writable == NJS_ATTRIBUTE_UNSET
            || njs_is_valid(&prop->value))
        {
            found = 1;
        }
    }

    njs_set_boolean(&vm->retval, found);

    return sizeof(njs_vmcode_3addr_t);
}


static njs_jump_off_t
njs_vmcode_property_delete(njs_vm_t *vm, njs_value_t *value, njs_value_t *key)
{
    njs_jump_off_t        ret;
    njs_object_prop_t     *prop;
    njs_property_query_t  pq;

    njs_property_query_init(&pq, NJS_PROPERTY_QUERY_DELETE, 1);

    ret = njs_property_query(vm, &pq, value, key);

    switch (ret) {

    case NJS_OK:
        prop = pq.lhq.value;

        if (njs_slow_path(!prop->configurable)) {
            njs_type_error(vm, "Cannot delete property \"%V\" of %s",
                           &pq.lhq.key, njs_type_string(value->type));
            return NJS_ERROR;
        }

        switch (prop->type) {
        case NJS_PROPERTY_HANDLER:
            if (njs_is_external(value)) {
                ret = prop->value.data.u.prop_handler(vm, value, NULL, NULL);
                if (njs_slow_path(ret != NJS_OK)) {
                    return ret;
                }

                goto done;
            }

            /* Fall through. */

        case NJS_PROPERTY:
            break;

        case NJS_PROPERTY_REF:
            njs_set_invalid(prop->value.data.u.value);
            goto done;

        default:
            njs_internal_error(vm, "unexpected property type \"%s\" "
                               "while deleting",
                               njs_prop_type_string(prop->type));

            return NJS_ERROR;
        }

        /* GC: release value. */
        prop->type = NJS_WHITEOUT;
        njs_set_invalid(&prop->value);

        break;

    case NJS_DECLINED:
        break;

    case NJS_ERROR:
    default:

        return ret;
    }

done:

    vm->retval = njs_value_true;

    return sizeof(njs_vmcode_3addr_t);
}


static njs_jump_off_t
njs_vmcode_property_foreach(njs_vm_t *vm, njs_value_t *object,
    njs_value_t *invld, u_char *pc)
{
    void                       *obj;
    njs_jump_off_t             ret;
    const njs_extern_t         *ext_proto;
    njs_property_next_t        *next;
    njs_vmcode_prop_foreach_t  *code;

    if (njs_is_external(object)) {
        ext_proto = object->external.proto;

        if (ext_proto->foreach != NULL) {
            obj = njs_extern_object(vm, object);

            ret = ext_proto->foreach(vm, obj, &vm->retval);
            if (njs_slow_path(ret != NJS_OK)) {
                return ret;
            }
        }

        goto done;
    }

    next = njs_mp_alloc(vm->mem_pool, sizeof(njs_property_next_t));
    if (njs_slow_path(next == NULL)) {
        njs_memory_error(vm);
        return NJS_ERROR;
    }

    next->index = 0;
    next->array = njs_value_enumerate(vm, object, NJS_ENUM_KEYS, 0);
    if (njs_slow_path(next->array == NULL)) {
        njs_memory_error(vm);
        return NJS_ERROR;
    }

    vm->retval.data.u.next = next;

done:

    code = (njs_vmcode_prop_foreach_t *) pc;

    return code->offset;
}


static njs_jump_off_t
njs_vmcode_property_next(njs_vm_t *vm, njs_value_t *object, njs_value_t *value,
    u_char *pc)
{
    void                    *obj;
    njs_value_t             *retval;
    njs_jump_off_t          ret;
    njs_property_next_t     *next;
    const njs_extern_t      *ext_proto;
    njs_vmcode_prop_next_t  *code;

    code = (njs_vmcode_prop_next_t *) pc;
    retval = njs_vmcode_operand(vm, code->retval);

    if (njs_is_external(object)) {
        ext_proto = object->external.proto;

        if (ext_proto->next != NULL) {
            obj = njs_extern_object(vm, object);

            ret = ext_proto->next(vm, retval, obj, value);

            if (ret == NJS_OK) {
                return code->offset;
            }

            if (njs_slow_path(ret == NJS_ERROR)) {
                return ret;
            }

            /* ret == NJS_DONE. */
        }

        return sizeof(njs_vmcode_prop_next_t);
    }

    next = value->data.u.next;

    if (next->index < next->array->length) {
        *retval = next->array->data[next->index++];

        return code->offset;
    }

    njs_mp_free(vm->mem_pool, next);

    return sizeof(njs_vmcode_prop_next_t);
}


static njs_jump_off_t
njs_vmcode_instance_of(njs_vm_t *vm, njs_value_t *object,
    njs_value_t *constructor)
{
    njs_value_t        value;
    njs_object_t       *prototype, *proto;
    njs_jump_off_t     ret;
    const njs_value_t  *retval;

    static njs_value_t prototype_string = njs_string("prototype");

    if (!njs_is_function(constructor)) {
        njs_type_error(vm, "right argument is not a function");
        return NJS_ERROR;
    }

    retval = &njs_value_false;

    if (njs_is_object(object)) {
        value = njs_value_undefined;
        ret = njs_value_property(vm, constructor, &prototype_string, &value);

        if (njs_slow_path(ret == NJS_ERROR)) {
            return ret;
        }

        if (njs_fast_path(ret == NJS_OK)) {

            if (njs_slow_path(!njs_is_object(&value))) {
                njs_internal_error(vm, "prototype is not an object");
                return NJS_ERROR;
            }

            prototype = njs_object(&value);
            proto = njs_object(object);

            do {
                proto = proto->__proto__;

                if (proto == prototype) {
                    retval = &njs_value_true;
                    break;
                }

            } while (proto != NULL);
        }
    }

    vm->retval = *retval;

    return sizeof(njs_vmcode_instance_of_t);
}


static njs_jump_off_t
njs_vmcode_typeof(njs_vm_t *vm, njs_value_t *value, njs_value_t *invld)
{
    /* ECMAScript 5.1: null, array and regexp are objects. */

    static const njs_value_t  *types[NJS_TYPE_MAX] = {
        &njs_string_object,
        &njs_string_undefined,
        &njs_string_boolean,
        &njs_string_number,
        &njs_string_string,
        &njs_string_data,
        &njs_string_external,
        &njs_string_invalid,
        &njs_string_undefined,
        &njs_string_undefined,
        &njs_string_undefined,
        &njs_string_undefined,
        &njs_string_undefined,
        &njs_string_undefined,
        &njs_string_undefined,
        &njs_string_undefined,

        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_function,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
        &njs_string_object,
    };

    vm->retval = *types[value->type];

    return sizeof(njs_vmcode_2addr_t);
}


static njs_jump_off_t
njs_string_concat(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    u_char             *start;
    size_t             size, length;
    njs_string_prop_t  string1, string2;

    (void) njs_string_prop(&string1, val1);
    (void) njs_string_prop(&string2, val2);

    /*
     * A result of concatenation of Byte and ASCII or UTF-8 strings
     * is a Byte string.
     */
    if ((string1.length != 0 || string1.size == 0)
        && (string2.length != 0 || string2.size == 0))
    {
        length = string1.length + string2.length;

    } else {
        length = 0;
    }

    size = string1.size + string2.size;

    start = njs_string_alloc(vm, &vm->retval, size, length);

    if (njs_slow_path(start == NULL)) {
        return NJS_ERROR;
    }

    (void) memcpy(start, string1.start, string1.size);
    (void) memcpy(start + string1.size, string2.start, string2.size);

    return sizeof(njs_vmcode_3addr_t);
}


static njs_jump_off_t
njs_values_equal(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    njs_int_t    ret;
    njs_bool_t   nv1, nv2;
    njs_value_t  primitive;
    njs_value_t  *hv, *lv;

again:

    nv1 = njs_is_null_or_undefined(val1);
    nv2 = njs_is_null_or_undefined(val2);

    /* Void and null are equal and not comparable with anything else. */
    if (nv1 || nv2) {
        return (nv1 && nv2);
    }

    if (njs_is_numeric(val1) && njs_is_numeric(val2)) {
        /* NaNs and Infinities are handled correctly by comparision. */
        return (njs_number(val1) == njs_number(val2));
    }

    if (val1->type == val2->type) {

        if (njs_is_string(val1)) {
            return njs_string_eq(val1, val2);
        }

        return (njs_object(val1) == njs_object(val2));
    }

    /* Sort values as: numeric < string < objects. */

    if (val1->type > val2->type) {
        hv = val1;
        lv = val2;

    } else {
        hv = val2;
        lv = val1;
    }

    /* If "lv" is an object then "hv" can only be another object. */
    if (njs_is_object(lv)) {
        return 0;
    }

    /* If "hv" is a string then "lv" can only be a numeric. */
    if (njs_is_string(hv)) {
        return (njs_number(lv) == njs_string_to_number(hv, 0));
    }

    /* "hv" is an object and "lv" is either a string or a numeric. */

    ret = njs_value_to_primitive(vm, &primitive, hv, 0);
    if (ret != NJS_OK) {
        return ret;
    }

    val1 = &primitive;
    val2 = lv;

    goto again;
}


/*
 * ECMAScript 5.1: 11.8.5
 * njs_primitive_values_compare() returns
 *   1 if val1 is less than val2,
 *   0 if val1 is greater than or equal to val2,
 *  -1 if the values are not comparable.
 */

static njs_jump_off_t
njs_primitive_values_compare(njs_vm_t *vm, njs_value_t *val1, njs_value_t *val2)
{
    double   num1, num2;

    if (njs_fast_path(njs_is_numeric(val1))) {
        num1 = njs_number(val1);

        if (njs_fast_path(njs_is_numeric(val2))) {
            num2 = njs_number(val2);

        } else {
            num2 = njs_string_to_number(val2, 0);
        }

    } else if (njs_is_numeric(val2)) {
        num1 = njs_string_to_number(val1, 0);
        num2 = njs_number(val2);

    } else {
        return (njs_string_cmp(val1, val2) < 0) ? 1 : 0;
    }

    /* NaN and void values are not comparable with anything. */
    if (isnan(num1) || isnan(num2)) {
        return -1;
    }

    /* Infinities are handled correctly by comparision. */
    return (num1 < num2);
}


static njs_jump_off_t
njs_function_frame_create(njs_vm_t *vm, njs_value_t *value,
    const njs_value_t *this, uintptr_t nargs, njs_bool_t ctor)
{
    njs_value_t     val;
    njs_object_t    *object;
    njs_function_t  *function;

    if (njs_fast_path(njs_is_function(value))) {

        function = njs_function(value);

        if (ctor) {
            if (!function->ctor) {
                njs_type_error(vm, "%s is not a constructor",
                               njs_type_string(value->type));
                return NJS_ERROR;
            }

            if (!function->native) {
                object = njs_function_new_object(vm, value);
                if (njs_slow_path(object == NULL)) {
                    return NJS_ERROR;
                }

                njs_set_object(&val, object);
                this = &val;
            }
        }

        return njs_function_frame(vm, function, this, NULL, nargs, ctor);
    }

    njs_type_error(vm, "%s is not a function", njs_type_string(value->type));

    return NJS_ERROR;
}


static njs_object_t *
njs_function_new_object(njs_vm_t *vm, njs_value_t *value)
{
    njs_value_t         *proto;
    njs_object_t        *object;
    njs_jump_off_t      ret;
    njs_function_t      *function;
    njs_object_prop_t   *prop;
    njs_lvlhsh_query_t  lhq;

    object = njs_object_alloc(vm);

    if (njs_fast_path(object != NULL)) {

        lhq.key_hash = NJS_PROTOTYPE_HASH;
        lhq.key = njs_str_value("prototype");
        lhq.proto = &njs_object_hash_proto;
        function = njs_function(value);

        ret = njs_lvlhsh_find(&function->object.hash, &lhq);

        if (ret == NJS_OK) {
            prop = lhq.value;
            proto = &prop->value;

        } else {
            proto = njs_function_property_prototype_create(vm, value);
        }

        if (njs_fast_path(proto != NULL)) {
            object->__proto__ = njs_object(proto);
            return object;
        }
   }

   return NULL;
}


static njs_jump_off_t
njs_vmcode_return(njs_vm_t *vm, njs_value_t *invld, njs_value_t *retval)
{
    njs_value_t         *value;
    njs_frame_t         *frame;
    njs_native_frame_t  *previous;

    value = njs_vmcode_operand(vm, retval);

    frame = (njs_frame_t *) vm->top_frame;

    if (frame->native.ctor) {
        if (njs_is_object(value)) {
            njs_release(vm, vm->scopes[NJS_SCOPE_ARGUMENTS]);

        } else {
            value = vm->scopes[NJS_SCOPE_ARGUMENTS];
        }
    }

    previous = njs_function_previous_frame(&frame->native);

    njs_vm_scopes_restore(vm, frame, previous);

    /*
     * If a retval is in a callee arguments scope it
     * must be in the previous callee arguments scope.
     */
    retval = njs_vmcode_operand(vm, frame->retval);

    /* GC: value external/internal++ depending on value and retval type */
    *retval = *value;

    njs_function_frame_free(vm, &frame->native);

    return NJS_OK;
}


/*
 * njs_vmcode_try_start() is set on the start of a "try" block to create
 * a "try" block, to set a catch address to the start of a "catch" or
 * "finally" blocks and to initialize a value to track uncaught exception.
 */

static njs_jump_off_t
njs_vmcode_try_start(njs_vm_t *vm, njs_value_t *exception_value,
    njs_value_t *offset, u_char *pc)
{
    njs_value_t             *exit_value;
    njs_exception_t         *e;
    njs_vmcode_try_start_t  *try_start;

    if (vm->top_frame->exception.catch != NULL) {
        e = njs_mp_alloc(vm->mem_pool, sizeof(njs_exception_t));
        if (njs_slow_path(e == NULL)) {
            njs_memory_error(vm);
            return NJS_ERROR;
        }

        *e = vm->top_frame->exception;
        vm->top_frame->exception.next = e;
    }

    vm->top_frame->exception.catch = pc + (njs_jump_off_t) offset;

    njs_set_invalid(exception_value);

    try_start = (njs_vmcode_try_start_t *) pc;
    exit_value = njs_vmcode_operand(vm, try_start->exit_value);

    njs_set_invalid(exit_value);
    njs_number(exit_value) = 0;

    return sizeof(njs_vmcode_try_start_t);
}


/*
 * njs_vmcode_try_break() sets exit_value to INVALID 1, and jumps to
 * the nearest try_end block. The exit_value is checked by njs_vmcode_finally().
 */

static njs_jump_off_t
njs_vmcode_try_break(njs_vm_t *vm, njs_value_t *exit_value,
    njs_value_t *offset)
{
    /* exit_value can contain valid value set by vmcode_try_return. */
    if (!njs_is_valid(exit_value)) {
        njs_number(exit_value) = 1;
    }

    return (njs_jump_off_t) offset;
}


/*
 * njs_vmcode_try_continue() sets exit_value to INVALID -1, and jumps to
 * the nearest try_end block. The exit_value is checked by njs_vmcode_finally().
 */

static njs_jump_off_t
njs_vmcode_try_continue(njs_vm_t *vm, njs_value_t *exit_value,
    njs_value_t *offset)
{
    njs_number(exit_value) = -1;

    return (njs_jump_off_t) offset;
}


/*
 * njs_vmcode_try_end() is set on the end of a "try" block to remove the block.
 * It is also set on the end of a "catch" block followed by a "finally" block.
 */

static njs_jump_off_t
njs_vmcode_try_end(njs_vm_t *vm, njs_value_t *invld, njs_value_t *offset)
{
    njs_exception_t  *e;

    e = vm->top_frame->exception.next;

    if (e == NULL) {
        vm->top_frame->exception.catch = NULL;

    } else {
        vm->top_frame->exception = *e;
        njs_mp_free(vm->mem_pool, e);
    }

    return (njs_jump_off_t) offset;
}


/*
 * njs_vmcode_finally() is set on the end of a "finally" or a "catch" block.
 *   1) to throw uncaught exception.
 *   2) to make a jump to an enslosing loop exit if "continue" or "break"
 *      statement was used inside try block.
 *   3) to finalize "return" instruction from "try" block.
 */

static njs_jump_off_t
njs_vmcode_finally(njs_vm_t *vm, njs_value_t *invld, njs_value_t *retval,
    u_char *pc)
{
    njs_value_t           *exception_value, *exit_value;
    njs_vmcode_finally_t  *finally;

    exception_value = njs_vmcode_operand(vm, retval);

    if (njs_is_valid(exception_value)) {
        vm->retval = *exception_value;

        return NJS_ERROR;
    }

    finally = (njs_vmcode_finally_t *) pc;
    exit_value = njs_vmcode_operand(vm, finally->exit_value);

    /*
     * exit_value is set by:
     *   vmcode_try_start to INVALID 0
     *   vmcode_try_break to INVALID 1
     *   vmcode_try_continue to INVALID -1
     *   vmcode_try_return to a valid return value
     */

    if (njs_is_valid(exit_value)) {
        return njs_vmcode_return(vm, NULL, exit_value);

    } else if (njs_number(exit_value) != 0) {
        return (njs_jump_off_t) (njs_number(exit_value) > 0)
                                ? finally->break_offset
                                : finally->continue_offset;
    }

    return sizeof(njs_vmcode_finally_t);
}


static void
njs_vmcode_reference_error(njs_vm_t *vm, u_char *pc)
{
    njs_str_t                     *file;
    njs_vmcode_reference_error_t  *ref_err;

    ref_err = (njs_vmcode_reference_error_t *) pc;

    file = &ref_err->file;

    if (file->length != 0 && !vm->options.quiet) {
        njs_reference_error(vm, "\"%V\" is not defined in %V:%uD",
                            &ref_err->name, file, ref_err->token_line);

    } else {
        njs_reference_error(vm, "\"%V\" is not defined in %uD", &ref_err->name,
                            ref_err->token_line);
    }
}
