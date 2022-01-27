/**@file 2c.c
 * @brief Convert the Abstract Syntax Tree generated by mpc for the DBC file
 * into some C code which can encode/decode signals.
 * @copyright Richard James Howe (2018)
 * @license MIT
 *
 * This file is quite a mess, but that is not going to change, it is also
 * quite short and seems to do the job. A better solution would be to make a
 * template tool, or a macro processor, suited for the task of generating C
 * code. The entire program really should be written in a language like Perl or
 * Python, but I wanted to use the MPC library for something, so here we are. */

#include "2c.h"
#include "util.h"
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#define MAX_NAME_LENGTH (512u)

/* The float packing and unpacking is stolen and modified from
 * <https://beej.us/guide/bgnet/examples/pack2b.c>!
 * (It's public domain code as far as I know, from Beej's guide to network
 * programming).
 *
 * The following link provides a calculator you can use to see what
 * bits correspond to a floating point number:
 * <https://www.h-schmidt.net/FloatConverter/IEEE754.html>
 *
 * Special cases:
 *
 * Zero and sign bit set -> Negative Zero
 *
 * All Exponent Bits Set
 * - Mantissa is zero and sign bit is zero ->  Infinity
 * - Mantissa is zero and sign bit is on   -> -Infinity
 * - Mantissa is non-zero -> NaN */


static char *float_pack = "\
/* pack754() -- pack a floating point number into IEEE-754 format */ \n\
static uint64_t pack754(const double f, const unsigned bits, const unsigned expbits) {\n\
	if (f == 0.0) /* get this special case out of the way */\n\
		return signbit(f) ? (1uLL << (bits - 1)) :  0;\n\
	if (f != f) /* NaN, encoded as Exponent == all-bits-set, Mantissa != 0, Signbit == Do not care */\n\
		return (1uLL << (bits - 1)) - 1uLL;\n\
	if (f == INFINITY) /* +INFINITY encoded as Mantissa == 0, Exponent == all-bits-set */\n\
		return ((1uLL << expbits) - 1uLL) << (bits - expbits - 1);\n\
	if (f == -INFINITY) /* -INFINITY encoded as Mantissa == 0, Exponent == all-bits-set, Signbit == 1 */\n\
		return (1uLL << (bits - 1)) | ((1uLL << expbits) - 1uLL) << (bits - expbits - 1);\n\
\n\
	long long sign = 0;\n\
	double fnorm = f;\n\
	/* check sign and begin normalization */\n\
	if (f < 0) { sign = 1; fnorm = -f; }\n\
\n\
	/* get the normalized form of f and track the exponent */\n\
	int shift = 0;\n\
	while (fnorm >= 2.0) { fnorm /= 2.0; shift++; }\n\
	while (fnorm < 1.0)  { fnorm *= 2.0; shift--; }\n\
	fnorm = fnorm - 1.0;\n\
\n\
	const unsigned significandbits = bits - expbits - 1; // -1 for sign bit\n\
\n\
	/* calculate the binary form (non-float) of the significand data */\n\
	const long long significand = fnorm * (( 1LL << significandbits) + 0.5f);\n\
\n\
	/* get the biased exponent */\n\
	const long long exp = shift + ((1LL << (expbits - 1)) - 1); // shift + bias\n\
\n\
	/* return the final answer */\n\
	return (sign << (bits - 1)) | (exp << (bits - expbits - 1)) | significand;\n\
}\n\
\n\
static inline uint32_t   pack754_32(const float  f)   { return   pack754(f, 32, 8); }\n\
static inline uint64_t   pack754_64(const double f)   { return   pack754(f, 64, 11); }\n\
\n\n";

static char *float_unpack = "\
/* unpack754() -- unpack a floating point number from IEEE-754 format */ \n\
static double unpack754(const uint64_t i, const unsigned bits, const unsigned expbits) {\n\
	if (i == 0) return 0.0;\n\
\n\
	const uint64_t expset = ((1uLL << expbits) - 1uLL) << (bits - expbits - 1);\n\
	if ((i & expset) == expset) { /* NaN or +/-Infinity */\n\
		if (i & ((1uLL << (bits - expbits - 1)) - 1uLL)) /* Non zero Mantissa means NaN */\n\
			return NAN;\n\
		return (i & (1uLL << (bits - 1))) ? -INFINITY : INFINITY;\n\
	}\n\
\n\
	/* pull the significand */\n\
	const unsigned significandbits = bits - expbits - 1; /* - 1 for sign bit */\n\
	double result = (i & ((1LL << significandbits) - 1)); /* mask */\n\
	result /= (1LL << significandbits);  /* convert back to float */\n\
	result += 1.0f;                        /* add the one back on */\n\
\n\
	/* deal with the exponent */\n\
	const unsigned bias = (1 << (expbits - 1)) - 1;\n\
	long long shift = ((i >> significandbits) & ((1LL << expbits) - 1)) - bias;\n\
	while (shift > 0) { result *= 2.0; shift--; }\n\
	while (shift < 0) { result /= 2.0; shift++; }\n\
	\n\
	return ((i >> (bits - 1)) & 1) ? -result : result; /* sign it, and return */\n\
}\n\
\n\
static inline float    unpack754_32(uint32_t i) { return unpack754(i, 32, 8); }\n\
static inline double   unpack754_64(uint64_t i) { return unpack754(i, 64, 11); }\n\
\n\n";



static const bool swap_motorola = true;

static unsigned fix_start_bit(bool motorola, unsigned start, unsigned siglen)
{
	if (motorola)
		start = (8 * (7 - (start / 8))) + (start % 8) - (siglen - 1);
	return start;
}

static const char *determine_unsigned_type(unsigned length)
{
	const char *type = "uint64_t";
	if (length <= 32)
		type = "uint32_t";
	if (length <= 16)
		type = "uint16_t";
	if (length <= 8)
		type = "uint8_t";
	return type;
}

static const char *determine_signed_type(unsigned length)
{
	const char *type = "int64_t";
	if (length <= 32)
		type = "int32_t";
	if (length <= 16)
		type = "int16_t";
	if (length <= 8)
		type = "int8_t";
	return type;
}

static const char *determine_type(unsigned length, bool is_signed, bool is_floating)
{
	if (is_floating)
		return length == 64 ? "double" : "float";
	return is_signed ?
		determine_signed_type(length) :
		determine_unsigned_type(length);
}

static int comment(signal_t *sig, FILE *o, const char *indent)
{
	assert(sig);
	assert(o);
	return fprintf(o, "%s/* %s: start-bit %u, length %u, endianess %s, scaling %g, offset %g */\n",
			indent,
			sig->name,
			sig->start_bit,
			sig->bit_length,
			sig->endianess == endianess_motorola_e ? "motorola" : "intel",
			sig->scaling,
			sig->offset);
}

static int signal2deserializer(signal_t *sig, const char *msg_name, FILE *o, const char *indent)
{
	assert(sig);
	assert(msg_name);
	assert(o);
	const bool motorola   = (sig->endianess == endianess_motorola_e);
	const unsigned start  = fix_start_bit(motorola, sig->start_bit, sig->bit_length);
	const unsigned length = sig->bit_length;
	const uint64_t mask = length == 64 ?
		0xFFFFFFFFFFFFFFFFuLL :
		(1uLL << length) - 1uLL;

	if (comment(sig, o, indent) < 0)
		return -1;

	if (start)
		fprintf(o, "%sx = (%c >> %d) & 0x%"PRIx64";\n", indent, motorola ? 'm' : 'i', start, mask);
	else
		fprintf(o, "%sx = %c & 0x%"PRIx64";\n", indent, motorola ? 'm' : 'i',  mask);

	if (sig->is_floating) {
		assert(length == 32 || length == 64);
		if (fprintf(o, "%so->%s.%s = unpack754_%d(x);\n", indent, msg_name, sig->name, length) < 0)
			return -1;
		return 0;
	}

	if (sig->is_signed) {
		const uint64_t top = (1uL << (length - 1));
		uint64_t negative = ~mask;
		if (length <= 32)
			negative &= 0xFFFFFFFF;
		if (length <= 16)
			negative &= 0xFFFF;
		if (length <= 8)
			negative &= 0xFF;
		if (negative)
			fprintf(o, "%sx = (x & 0x%"PRIx64") ? (x | 0x%"PRIx64") : x; \n", indent, top, negative);
	}

	fprintf(o, "%so->%s.%s = x;\n", indent, msg_name, sig->name);
	return 0;
}

static int signal2serializer(signal_t *sig, const char *msg_name, FILE *o, const char *indent)
{
	assert(sig);
	assert(o);
	bool motorola = (sig->endianess == endianess_motorola_e);
	int start = fix_start_bit(motorola, sig->start_bit, sig->bit_length);

	uint64_t mask = sig->bit_length == 64 ?
		0xFFFFFFFFFFFFFFFFuLL :
		(1uLL << sig->bit_length) - 1uLL;

	if (comment(sig, o, indent) < 0)
		return -1;

	if (sig->is_floating) {
		assert(sig->bit_length == 32 || sig->bit_length == 64);
		fprintf(o, "%sx = pack754_%u(o->%s.%s) & 0x%"PRIx64";\n", indent, sig->bit_length, msg_name, sig->name, mask);
	} else {
		fprintf(o, "%sx = ((%s)(o->%s.%s)) & 0x%"PRIx64";\n", indent, determine_unsigned_type(sig->bit_length), msg_name, sig->name, mask);
	}
	if (start)
		fprintf(o, "%sx <<= %u; \n", indent, start);
	fprintf(o, "%s%c |= x;\n", indent, motorola ? 'm' : 'i');
	return 0;
}

static int signal2print(signal_t *sig, unsigned id, const char *msg_name, FILE *o)
{
	UNUSED(id);
	/*super lazy*/
	if (sig->is_floating)
		return fprintf(o, "\tr = print_helper(r, fprintf(output, \"%s = (wire: %%g)\\n\", (double)(o->%s.%s)));\n", sig->name, msg_name, sig->name);
	return fprintf(o, "\tr = print_helper(r, fprintf(output, \"%s = (wire: %%.0f)\\n\", (double)(o->%s.%s)));\n", sig->name, msg_name, sig->name);
}

static int signal2type(signal_t *sig, FILE *o)
{
	assert(sig);
	assert(o);
	const unsigned length = sig->bit_length;
	const char *type = determine_type(length, sig->is_signed, sig->is_floating);

	if (length == 0) {
		warning("signal %s has bit length of 0 (fix the dbc file)");
		return -1;
	}

	if (sig->is_floating) {
		if (length != 32 && length != 64) {
			warning("signal %s is floating point number but has length %u (fix the dbc file)", sig->name, length);
			return -1;
		}
	}

	if (sig->comment) {
		fprintf(o, "\t/* %s: %s */\n", sig->name, sig->comment);
		return fprintf(o, "\t/* scaling %.1f, offset %.1f, units %s %s */\n\t%s %s;\n",
				sig->scaling, sig->offset, sig->units[0] ? sig->units : "none",
				sig->is_floating ? ", floating" : "",
				type, sig->name);
	} else {
		return fprintf(o, "\t%s %s; /* scaling %.1f, offset %.1f, units %s %s */\n",
				type, sig->name, sig->scaling, sig->offset, sig->units[0] ? sig->units : "none",
				sig->is_floating ? ", floating" : "");
	}
}

static bool signal_are_min_max_valid(signal_t *sig)
{
	assert(sig);
	return sig->minimum != sig->maximum;
}

static uint64_t unsigned_max(signal_t *sig)
{
	assert(sig);
	if (sig->bit_length == 64)
		return UINT64_MAX;
	return (1uLL << (sig->bit_length)) - 1uLL;
}

static int64_t signed_max(signal_t *sig)
{
	assert(sig);
	if (sig->bit_length == 64)
		return INT64_MAX;
	return ((1uLL << (sig->bit_length - 1)) - 1uLL);
}

static int64_t signed_min(signal_t *sig)
{
	assert(sig);
	if (sig->bit_length == 64)
		return INT64_MIN;
	return ~signed_max(sig);
}

static int signal2scaling_encode(const char *msgname, unsigned id, signal_t *sig, FILE *o, bool header, const char *god, dbc2c_options_t *copts)
{
	assert(msgname);
	assert(sig);
	assert(o);
	assert(copts);
	const char *type = determine_type(sig->bit_length, sig->is_signed, sig->is_floating);
	if (sig->scaling != 1.0 || sig->offset != 0.0)
		type = "double";
	if (copts->use_id_in_name)
		fprintf(o, "int Can_Encode_%s_0x%03x_%s(Can_%s_t *o, %s in)", god, id, sig->name, god, copts->use_doubles_for_encoding ? "double" : type);
	else
		fprintf(o, "int Can_Encode_%s_%s(Can_%s_t *o, %s in)", god, sig->name, god, copts->use_doubles_for_encoding ? "double" : type);

	if (header)
		return fputs(";\n", o);
	fputs(" {\n", o);
	if (copts->generate_asserts) {
		fputs("\tassert(o);\n", o);
	}
	if (signal_are_min_max_valid(sig)) {
		bool gmax = true;
		bool gmin = true;

		if (sig->is_signed) {
			gmin = sig->minimum > signed_min(sig);
			gmax = sig->maximum < signed_max(sig);
		} else {
			gmin = sig->minimum > 0.0;
			gmax = sig->maximum < unsigned_max(sig);
		}
		if (sig->is_floating) {
			gmax = true;
			gmax = true;
		}

		if (gmin || gmax)
			fprintf(o, "\to->%s.%s = 0;\n", msgname, sig->name); // cast!
		if (gmin)
			fprintf(o, "\tif (in < %g)\n\t\treturn -1;\n", sig->minimum);
		if (gmax)
			fprintf(o, "\tif (in > %g)\n\t\treturn -1;\n", sig->maximum);
	}

	if (sig->scaling == 0.0)
		error("invalid scaling factor (fix your DBC file)");
	if (sig->offset != 0.0)
		fprintf(o, "\tin += %g;\n", -1.0 * sig->offset);
	if (sig->scaling != 1.0)
		fprintf(o, "\tin *= %g;\n", 1.0 / sig->scaling);
	fprintf(o, "\to->%s.%s = in;\n", msgname, sig->name); // cast!
	return fputs("\treturn 0;\n}\n\n", o);
}

static int signal2scaling_decode(const char *msgname, unsigned id, signal_t *sig, FILE *o, bool header, const char *god, dbc2c_options_t *copts)
{
	assert(msgname);
	assert(sig);
	assert(o);
	assert(copts);
	const char *type = determine_type(sig->bit_length, sig->is_signed, sig->is_floating);
	if (sig->scaling != 1.0 || sig->offset != 0.0)
		type = "double";
	if (copts->use_id_in_name)
		fprintf(o, "int Can_Decode_%s_0x%03x_%s(const Can_%s_t *o, %s *out)", god, id, sig->name, god, copts->use_doubles_for_encoding ? "double" : type);
	else
		fprintf(o, "int Can_Decode_%s_%s(const Can_%s_t *o, %s *out)", god, sig->name, god, copts->use_doubles_for_encoding ? "double" : type);
	if (header)
		return fputs(";\n", o);
	fputs(" {\n", o);
	if (copts->generate_asserts) {
		fputs("\tassert(o);\n", o);
		fputs("\tassert(out);\n", o);
	}
	fprintf(o, "\t%s rval = (%s)(o->%s.%s);\n", type, type, msgname, sig->name);
	if (sig->scaling == 0.0)
		error("invalid scaling factor (fix your DBC file)");
	if (sig->scaling != 1.0)
		fprintf(o, "\trval *= %g;\n", sig->scaling);
	if (sig->offset != 0.0)
		fprintf(o, "\trval += %g;\n", sig->offset);
	if (signal_are_min_max_valid(sig)) {
		bool gmax = true;
		bool gmin = true;

		if (sig->is_signed) { /**@warning comparison may fail because of limits of double size */
			gmin = sig->minimum > signed_min(sig);
			gmax = sig->maximum < signed_max(sig);
		} else {
			gmin = sig->minimum > 0.0;
			gmax = sig->maximum < unsigned_max(sig);
		}
		if (sig->is_floating) {
			gmax = true;
			gmax = true;
		}

		if (!gmax && !gmin) {
			fputs("\t*out = rval;\n", o);
			fputs("\treturn 0;\n", o);
		} else {
			if (gmin && gmax) {
				fprintf(o, "\tif ((rval >= %g) && (rval <= %g)) {\n", sig->minimum, sig->maximum);
			} else if (gmax) {
				fprintf(o, "\tif (rval <= %g) {\n", sig->maximum);
			} else if (gmin) {
				fprintf(o, "\tif (rval >= %g) {\n", sig->minimum);
			}
			fputs("\t\t*out = rval;\n", o);
			fputs("\t\treturn 0;\n", o);
			fputs("\t} else {\n", o);
			fprintf(o, "\t\t*out = (%s)0;\n", type);
			fputs("\t\treturn -1;\n", o);
			fputs("\t}\n", o);

		}


	} else {
		fputs("\t*out = rval;\n", o);
		fputs("\treturn 0;\n", o);
	}
	return fputs("}\n\n", o);
}

static int signal2scaling(const char *msgname, unsigned id, signal_t *sig, FILE *o, bool decode, bool header, const char *god, dbc2c_options_t *copts)
{
	assert(copts);
	if (decode)
		return signal2scaling_decode(msgname, id, sig, o, header, god, copts);
	return signal2scaling_encode(msgname, id, sig, o, header, god, copts);
}

static int print_function_name(FILE *out, const char *prefix, const char *name, const char *postfix, bool in, char *datatype, bool dlc, const char *god)
{
	assert(out);
	assert(prefix);
	assert(name);
	assert(god);
	assert(postfix);
	return fprintf(out, "static int %s_%s(Can_%s_t *o, %s %sdata%s)%s",
			prefix, name, god, datatype,
			in ? "" : "*",
			dlc ? ", uint8_t dlc, dbcc_time_stamp_t time_stamp" : "",
			postfix);
}

static void make_name(char *newname, size_t maxlen, const char *name, unsigned id, dbc2c_options_t *copts)
{
	assert(newname);
	assert(name);
	assert(copts);
	if (copts->use_id_in_name)
		snprintf(newname, maxlen-1, "Can_0x%03x_%s", id, name);
	else
		snprintf(newname, maxlen-1, "Can_%s", name);
}

static signal_t *find_multiplexor(can_msg_t *msg) {
	assert(msg);
	signal_t *multiplexor = NULL;
	for (size_t i = 0; i < msg->signal_count; i++) {
		signal_t *sig = msg->sigs[i];
		if (sig->is_multiplexor) {
			if (multiplexor)
				error("multiple multiplexor values detected (only one per CAN msg is allowed) for %s", msg->name);
			multiplexor = sig;
		}
		if (sig->is_multiplexed)
			continue;
	}
	return multiplexor;
}

static signal_t *process_signals_and_find_multiplexer(can_msg_t *msg, FILE *c, const char *name, bool serialize)
{
	assert(msg);
	assert(c);
	assert(name);
	signal_t *multiplexor = NULL;

	for (size_t i = 0; i < msg->signal_count; i++) {
		signal_t *sig = msg->sigs[i];
		if (sig->is_multiplexor) {
			if (multiplexor)
				error("multiple multiplexor values detected (only one per CAN msg is allowed) for %s", name);
			multiplexor = sig;
		}
		if (sig->is_multiplexed)
			continue;
		if ((serialize ? signal2serializer(sig, name, c, "\t") : signal2deserializer(sig, name, c, "\t")) < 0)
			error("%s failed", serialize ? "serialization" : "deserialization");
	}
	return multiplexor;
}

static int cmp_signal(const void *lhs, const void *rhs)
{
	assert(lhs);
	assert(rhs);
	int ret = 0;
	if ((*(signal_t**)lhs)->switchval < ((*(signal_t**)rhs)->switchval))
		ret = -1;
	else if ((*(signal_t**)lhs)->switchval > (*(signal_t**)rhs)->switchval)
		ret = 1;
	return ret;
}
static int multiplexor_switch(can_msg_t *msg, signal_t *multiplexor, FILE *c, const char *msg_name, bool serialize)
{
	assert(msg);
	assert(multiplexor);
	assert(c);
	fprintf(c, "\tswitch (o->%s.%s) {\n", msg_name, multiplexor->name);
	qsort(msg->sigs, msg->signal_count, sizeof(*msg->sigs), cmp_signal);
	for (size_t i = 0; i < msg->signal_count; i++) {
		signal_t *sig = msg->sigs[i];
		if (!(sig->is_multiplexed))
			continue;
		fprintf(c, "\tcase %u:\n", sig->switchval);
		size_t j = i;
		for (; j < msg->signal_count && msg->sigs[i]->switchval == msg->sigs[j]->switchval; j++) {
			assert(j < msg->signal_count);
			signal_t* sig = msg->sigs[j];
			if ((serialize ? signal2serializer(sig, msg_name, c, "\t\t") : signal2deserializer(sig, msg_name, c, "\t\t")) < 0)
				return -1;
		}
		i = j - 1;
		assert(i < msg->signal_count);
		fprintf(c, "\t\tbreak;\n");
	}
	fprintf(c, "\tdefault:\n\t\treturn -1;\n\t}\n");
	return 0;
}

static int msg_data_type(FILE *c, can_msg_t *msg, bool data, dbc2c_options_t *copts)
{
	assert(c);
	assert(msg);
	assert(copts);
	char name[MAX_NAME_LENGTH] = {0};
	make_name(name, MAX_NAME_LENGTH, msg->name, msg->id, copts);
	return fprintf(c, "\t%s_t %s%s;\n", name, name, data ? "_data" : "");
}


static int msg_data_type_bitfields(FILE *c, can_msg_t *msg, dbc2c_options_t *copts) {
	assert(c);
	assert(msg);
	assert(copts);
	char name[MAX_NAME_LENGTH] = {0};
	make_name(name, MAX_NAME_LENGTH, msg->name, msg->id, copts);
	fprintf(c, "\tunsigned %s_status : 2;\n", name); /* uninitialized, present, faulty (range/crc/timeout/other) */
	fprintf(c, "\tunsigned %s_tx : 1;\n", name); /* have we packed this message? */
	return fprintf(c, "\tunsigned %s_rx : 1;\n", name); /* have we unpacked this message? */
}

static int msg_data_type_time_stamp(FILE *c, can_msg_t *msg, dbc2c_options_t *copts) {
	assert(c);
	assert(msg);
	assert(copts);
	char name[MAX_NAME_LENGTH] = {0};
	make_name(name, MAX_NAME_LENGTH, msg->name, msg->id, copts);
	return fprintf(c, "\tdbcc_time_stamp_t %s_time_stamp_rx;\n", name);
}

static int msg_pack(can_msg_t *msg, FILE *c, const char *name, bool motorola_used, bool intel_used, const char *god, dbc2c_options_t *copts)
{
	assert(msg);
	assert(c);
	assert(name);
	assert(copts);
	const bool message_has_signals = motorola_used || intel_used;
	print_function_name(c, "Pack", name, " {\n", false, "uint64_t", false, god);
	if (copts->generate_asserts) {
		fprintf(c, "\tassert(o);\n");
		fprintf(c, "\tassert(data);\n");
	}
	if (message_has_signals)
		fprintf(c, "\tregister uint64_t x;\n");
	if (motorola_used)
		fprintf(c, "\tregister uint64_t m = 0;\n");
	if (intel_used)
		fprintf(c, "\tregister uint64_t i = 0;\n");
	if (!message_has_signals)
		fprintf(c, "\tUNUSED(o);\n\tUNUSED(data);\n");
	signal_t *multiplexor = process_signals_and_find_multiplexer(msg, c, name, true);

	if (multiplexor)
		if (multiplexor_switch(msg, multiplexor, c, name, true) < 0)
			return -1;

	if (message_has_signals) {
		fprintf(c, "\t*data = %s%s%s%s%s;\n",
			swap_motorola && motorola_used ? "reverse_byte_order" : "",
			motorola_used ? "(m)" : "",
			motorola_used && intel_used ? "|" : "",
			(!swap_motorola && intel_used) ? "reverse_byte_order" : "",
			intel_used ? "(i)" : "");
	}
	fprintf(c, "\to->%s_tx = 1;\n", name);
	fprintf(c, "\treturn 0;\n}\n\n");
	return 0;
}

static int msg_unpack(can_msg_t *msg, FILE *c, const char *name, bool motorola_used, bool intel_used, const char *god, dbc2c_options_t *copts)
{
	assert(msg);
	assert(c);
	assert(name);
	assert(copts);
	const bool message_has_signals = motorola_used || intel_used;
	print_function_name(c, "Unpack", name, " {\n", true, "uint64_t", true, god);
	if (copts->generate_asserts) {
		fprintf(c, "\tassert(o);\n");
		fprintf(c, "\tassert(dlc <= 8);\n");
	}
	if (message_has_signals)
		fprintf(c, "\tregister uint64_t x;\n");
	if (motorola_used)
		fprintf(c, "\tregister uint64_t m = %s(data);\n", swap_motorola ? "reverse_byte_order" : "");
	if (intel_used)
		fprintf(c, "\tregister uint64_t i = %s(data);\n", swap_motorola ? "" : "reverse_byte_order");
	if (!message_has_signals)
		fprintf(c, "\tUNUSED(o);\n\tUNUSED(data);\n");
	if (msg->dlc)
		fprintf(c, "\tif (dlc < %u)\n\t\treturn -1;\n", msg->dlc);
	else
		fprintf(c, "\tUNUSED(dlc);\n");

	signal_t *multiplexor = process_signals_and_find_multiplexer(msg, c, name, false);
	if (multiplexor)
		if (multiplexor_switch(msg, multiplexor, c, name, false) < 0)
			return -1;
	fprintf(c, "\to->%s_rx = 1;\n", name);
	fprintf(c, "\to->%s_time_stamp_rx = time_stamp;\n", name);
	fprintf(c, "\treturn 0;\n}\n\n");
	return 0;
}

static int msg_print(can_msg_t *msg, FILE *c, const char *name, const char *god, dbc2c_options_t *copts)
{
	assert(msg);
	assert(c);
	assert(name);
	assert(god);
	assert(copts);
	fprintf(c, "int print_%s(const Can_%s_t *o, FILE *output) {\n", name, god);
	if (copts->generate_asserts) {
		fputs("\tassert(o);\n", c);
		fputs("\tassert(output);\n", c);
		/* you may note the UNUSED macro may be generated, we should
		 * still assert we are passed the correct things */
	}
	if (msg->signal_count)
		fprintf(c, "\tint r = 0;\n"); //fprintf(c, "\tdouble scaled;\n\tint r = 0;\n");
	else
		fprintf(c, "\tUNUSED(o);\n\tUNUSED(output);\n");
	for (size_t i = 0; i < msg->signal_count; i++) {
		if (signal2print(msg->sigs[i], msg->id, name, c) < 0)
			return -1;
	}
	if (msg->signal_count)
		fprintf(c, "\treturn r;\n}\n\n");
	else
		fprintf(c, "\treturn 0;\n}\n\n");
	return 0;
}

static int msg_dlc_check(can_msg_t *msg) {
	assert(msg);
	const unsigned bits = msg->dlc * 8;
	unsigned used = 0;
	if (find_multiplexor(msg)) // skip multiplexed messages for now
		return 0;
	for (size_t i = 0; i < msg->signal_count; i++) {
		signal_t *s = msg->sigs[i];
		used += s->bit_length;
	}
	if (used > bits) {
		warning("Too many signals, not enough bytes (DLC is too low, fix your DBC file): %s", msg->name);
		return -1;
	}
	return 0;
}

static int msg2c(can_msg_t *msg, FILE *c, dbc2c_options_t *copts, char *god)
{
	assert(msg);
	assert(c);
	assert(copts);
	assert(god);
	char name[MAX_NAME_LENGTH] = {0};
	make_name(name, MAX_NAME_LENGTH, msg->name, msg->id, copts);
	bool motorola_used = false;
	bool intel_used = false;

	for (size_t i = 0; i < msg->signal_count; i++)
		if (msg->sigs[i]->endianess == endianess_motorola_e)
			motorola_used = true;
		else
			intel_used = true;

	/* sanity checks against messages should go here, we could check for;
	 * - odd min/max values given scaling
	 * - duplicate signals and messages
	 * They really should go into a semantic analysis phase after reading
	 * in the DBC file and parsing it. Oh Well. */
	msg_dlc_check(msg);

	if (copts->generate_pack && msg_pack(msg, c, name, motorola_used, intel_used, god, copts) < 0)
		return -1;

	if (copts->generate_unpack && msg_unpack(msg, c, name, motorola_used, intel_used, god, copts) < 0)
		return -1;

	for (size_t i = 0; i < msg->signal_count; i++) {
		if (copts->generate_unpack)
			if (signal2scaling(name, msg->id, msg->sigs[i], c, true, false, god, copts) < 0)
				return -1;
		if (copts->generate_pack)
			if (signal2scaling(name, msg->id, msg->sigs[i], c, false, false, god, copts) < 0)
				return -1;
	}

	if (copts->generate_print && msg_print(msg, c, name, god, copts) < 0)
		return -1;

	return 0;
}

static int msg2h(can_msg_t *msg, FILE *h, dbc2c_options_t *copts, const char *god)
{
	assert(msg);
	assert(h);
	assert(copts);
	assert(god);
	char name[MAX_NAME_LENGTH] = {0};
	make_name(name, MAX_NAME_LENGTH, msg->name, msg->id, copts);

	for (size_t i = 0; i < msg->signal_count; i++) {
		if (copts->generate_unpack)
			if (signal2scaling(name, msg->id, msg->sigs[i], h, true, true, god, copts) < 0)
				return -1;
		if (copts->generate_pack)
			if (signal2scaling(name, msg->id, msg->sigs[i], h, false, true, god, copts) < 0)
				return -1;
	}
	fputs("\n\n", h);
	return 0;
}

static const char *cfunctions =
"static inline uint64_t reverse_byte_order(uint64_t x) {\n"
"\tx = (x & 0x00000000FFFFFFFF) << 32 | (x & 0xFFFFFFFF00000000) >> 32;\n"
"\tx = (x & 0x0000FFFF0000FFFF) << 16 | (x & 0xFFFF0000FFFF0000) >> 16;\n"
"\tx = (x & 0x00FF00FF00FF00FF) << 8  | (x & 0xFF00FF00FF00FF00) >> 8;\n"
"\treturn x;\n"
"}\n\n";
static const char *cfunctions_print_only =
"static inline int print_helper(int r, int print_return_value) {\n"
"\treturn ((r >= 0) && (print_return_value >= 0)) ? r + print_return_value : -1;\n"
"}\n\n";

static int message_compare_function(const void *a, const void *b)
{
	assert(a);
	assert(b);
	can_msg_t *ap = *((can_msg_t**)a);
	can_msg_t *bp = *((can_msg_t**)b);
	if (ap->id <  bp->id) return -1;
	if (ap->id == bp->id) return  0;
	if (ap->id >  bp->id) return  1;
	return 0;
}

static int signal_compare_function(const void *a, const void *b)
{
	assert(a);
	assert(b);
	signal_t *ap = *((signal_t**)a);
	signal_t *bp = *((signal_t**)b);
	if (ap->bit_length <  bp->bit_length) return  1;
	if (ap->bit_length == bp->bit_length) return  0;
	if (ap->bit_length >  bp->bit_length) return -1;
	return 0;
}

static int switch_function(FILE *c, dbc_t *dbc, char *function, bool unpack,
		bool prototype, const char *datatype, bool dlc, const char *god, dbc2c_options_t *copts)
{
	assert(c);
	assert(dbc);
	assert(function);
	assert(god);
	assert(copts);
	fprintf(c, "int Can_%s_%s_message(Can_%s_t *o, const unsigned long id, %s %sdata%s)",
			god, function, god, datatype, unpack ? "" : "*",
			dlc ? ", uint8_t dlc, dbcc_time_stamp_t time_stamp" : "");
	if (prototype)
		return fprintf(c, ";\n");
	fprintf(c, " {\n");
	if (copts->generate_asserts) {
		fprintf(c, "\tassert(o);\n");
		fprintf(c, "\tassert(id < (1ul << 29)); /* 29-bit CAN ID is largest possible */\n");
		if (dlc)
			fprintf(c, "\tassert(dlc <= 8);         /* Maximum of 8 bytes in a CAN packet */\n");
	}

	fprintf(c, "\tswitch (id) {\n");
	for (size_t i = 0; i < dbc->message_count; i++) {
		can_msg_t *msg = dbc->messages[i];
		char name[MAX_NAME_LENGTH] = {0};
		make_name(name, MAX_NAME_LENGTH, msg->name, msg->id, copts);
		fprintf(c, "\tcase 0x%03lx: return %s_%s(o, data%s);\n",
				msg->id,
				function,
				name,
				dlc ? ", dlc, time_stamp" : "");
	}
	fprintf(c, "\tdefault: break; \n\t}\n");
	return fprintf(c, "\treturn -1; \n}\n\n");
}

static int switch_function_print(FILE *c, dbc_t *dbc, bool prototype, const char *god, dbc2c_options_t *copts)
{
	assert(c);
	assert(dbc);
	assert(god);
	assert(copts);
	fprintf(c, "int print_message(const Can_%s_t *o, const unsigned long id, FILE *output)", god);
	if (prototype)
		return fprintf(c, ";\n");
	fprintf(c, " {\n");
	if (copts->generate_asserts) {
		fprintf(c, "\tassert(o);\n");
		fprintf(c, "\tassert(id < (1ul << 29)); /* 29-bit CAN ID is largest possible */\n");
		fprintf(c, "\tassert(output);\n");
	}

	fprintf(c, "\tswitch (id) {\n");
	for (size_t i = 0; i < dbc->message_count; i++) {
		can_msg_t *msg = dbc->messages[i];
		char name[MAX_NAME_LENGTH] = {0};
		make_name(name, MAX_NAME_LENGTH, msg->name, msg->id, copts);
		fprintf(c, "\tcase 0x%03lx: return print_%s(o, output);\n", msg->id, name);
	}
	fprintf(c, "\tdefault: break; \n\t}\n");
	return fprintf(c, "\treturn -1; \n}\n\n");
}

static int msg2h_types(dbc_t *dbc, FILE *h, dbc2c_options_t *copts)
{
	assert(h);
	assert(dbc);
	assert(copts);

	for (size_t i = 0; i < dbc->message_count; i++) {
		can_msg_t *msg = dbc->messages[i];
		char name[MAX_NAME_LENGTH] = {0};
		make_name(name, MAX_NAME_LENGTH, msg->name, msg->id, copts);

		if (msg->comment)
			fprintf(h, "/* %s */\n", msg->comment);

		fprintf(h, "typedef PREPACK struct {\n" );
		for (size_t i = 0; i < msg->signal_count; i++)
			if (signal2type(msg->sigs[i], h) < 0)
				return -1;
		fprintf(h, "} POSTPACK %s_t;\n\n", name);
	}
	return 0;
}

static char *msg2h_god_object(dbc_t *dbc, FILE *h, const char *name, dbc2c_options_t *copts)
{
	assert(h);
	assert(dbc);
	assert(copts);
	char *object_name = duplicate(name);
	const size_t object_name_len = strlen(object_name);
	for (size_t i = 0; i < object_name_len; i++)
		object_name[i] = (isalnum(object_name[i])) ?  tolower(object_name[i]) : '_';
	fprintf(h, "typedef PREPACK struct {\n");
	for (size_t i = 0; i < dbc->message_count; i++)
		if (msg_data_type_time_stamp(h, dbc->messages[i], copts) < 0)
			goto fail;
	for (size_t i = 0; i < dbc->message_count; i++)
		if (msg_data_type_bitfields(h, dbc->messages[i], copts) < 0)
			goto fail;
	for (size_t i = 0; i < dbc->message_count; i++)
		if (msg_data_type(h, dbc->messages[i], false, copts) < 0)
			goto fail;
	fprintf(h, "} POSTPACK Can_%s_t;\n\n", object_name);
	return object_name;
fail:
	free(object_name);
	return NULL;
}

int dbc2c(dbc_t *dbc, FILE *c, FILE *h, const char *name, dbc2c_options_t *copts)
{
	assert(dbc);
	assert(c);
	assert(h);
	assert(name);
	assert(copts);
	int rv = 0;
	time_t rawtime = time(NULL);
	struct tm *timeinfo = localtime(&rawtime); /* This is not considered safe on Visual Studio */
	char *god = NULL;
	char *file_guard = duplicate(name);
	const size_t file_guard_len = strlen(file_guard);

	/* make file guard all upper case alphanumeric only, first character
	 * alpha only*/
	if (!isalpha(file_guard[0]))
		file_guard[0] = '_';
	for (size_t i = 0; i < file_guard_len; i++)
		file_guard[i] = (isalnum(file_guard[i])) ?  toupper(file_guard[i]) : '_';

	/* sort signals by id */
	qsort(dbc->messages, dbc->message_count, sizeof(dbc->messages[0]), message_compare_function);

	/* sort by size for better struct packing */
	for (size_t i = 0; i < dbc->message_count; i++) {
		can_msg_t *msg = dbc->messages[i];
		qsort(msg->sigs, msg->signal_count, sizeof(msg->sigs[0]), signal_compare_function);
	}

	/* header file (begin) */
	fprintf(h, "/** CAN message encoder/decoder: automatically generated - do not edit\n");
	if (copts->use_time_stamps)
		fprintf(h, "  * @note  Generated on %s", asctime(timeinfo));

	fprintf(h,
		"  * Generated by dbcc: See https://github.com/howerj/dbcc */\n"
		"#ifndef %s\n"
		"#define %s\n\n"
		"#include <stdint.h>\n"
		"%s\n\n"
		"#ifdef __cplusplus\n"
		"extern \"C\" { \n"
		"#endif\n\n",
		file_guard,
		file_guard,
		copts->generate_print   ? "#include <stdio.h>"  : "");

	fprintf(h, "#ifndef PREPACK\n");
	fprintf(h, "#define PREPACK\n");
	fprintf(h, "#endif\n\n");

	fprintf(h, "#ifndef POSTPACK\n");
	fprintf(h, "#define POSTPACK\n");
	fprintf(h, "#endif\n\n");

	fprintf(h, "#ifndef DBCC_TIME_STAMP\n");
	fprintf(h, "#define DBCC_TIME_STAMP\n");
	fprintf(h, "typedef uint32_t dbcc_time_stamp_t; /* Time stamp for message; you decide on units */\n");
	fprintf(h, "#endif\n\n");

	fprintf(h, "#ifndef DBCC_STATUS_ENUM\n");
	fprintf(h, "#define DBCC_STATUS_ENUM\n");
	fprintf(h, "typedef enum {\n");
	fprintf(h, "\tDBCC_SIG_STAT_UNINITIALIZED_E = 0, /* Message never sent/received */\n");
	fprintf(h, "\tDBCC_SIG_STAT_OK_E            = 1, /* Message ok */\n");
	fprintf(h, "\tDBCC_SIG_STAT_ERROR_E         = 2, /* Encode/Decode/Timestamp/Any error */\n");
	fprintf(h, "} dbcc_signal_status_e;\n");
	fprintf(h, "#endif\n\n");

	if (msg2h_types(dbc, h, copts) < 0) {
		rv = -1;
		goto fail;
	}

	god = msg2h_god_object(dbc, h, name, copts);
	if (!god) {
		rv = -1;
		goto fail;
	}

	if (copts->generate_unpack)
		switch_function(h, dbc, "Unpack", true, true, "uint64_t", true, god, copts);

	if (copts->generate_pack)
		switch_function(h, dbc, "Pack", false, true, "uint64_t", false, god, copts);

	if (copts->generate_print)
		switch_function_print(h, dbc, true, god, copts);

	fputs("\n", h);

	for (size_t i = 0; i < dbc->message_count; i++)
		if (msg2h(dbc->messages[i], h, copts, god) < 0)
			return -1;

	fputs(
		"#ifdef __cplusplus\n"
		"} \n"
		"#endif\n\n"
		"#endif\n",
		h);
	/* header file (end) */

	/* C FILE */
	fputs("/* Generated by DBCC, see <https://github.com/howerj/dbcc> */\n", c);
	fprintf(c, "#include \"%s\"\n", name);
	fprintf(c, "#include <inttypes.h>\n");
	if (dbc->use_float)
		fprintf(c, "#include <math.h> /* uses macros NAN, INFINITY, signbit, no need for -lm */\n");
	if (copts->generate_asserts)
		fprintf(c, "#include <assert.h>\n");
	fputc('\n', c);
	fprintf(c, "#define UNUSED(X) ((void)(X))\n\n");
	fputs(cfunctions, c);
	if (copts->generate_print)
		fputs(cfunctions_print_only, c);

	if (copts->generate_unpack && dbc->use_float)
		fputs(float_unpack, c);
	if (copts->generate_pack && dbc->use_float)
		fputs(float_pack, c);

	for (size_t i = 0; i < dbc->message_count; i++)
		if (msg2c(dbc->messages[i], c, copts, god) < 0) {
			rv = -1;
			goto fail;
		}

	if (copts->generate_unpack)
		switch_function(c, dbc, "Unpack", true, false, "uint64_t", true, god, copts);

	if (copts->generate_pack)
		switch_function(c, dbc, "Pack", false, false, "uint64_t", false, god, copts);

	if (copts->generate_print)
		switch_function_print(c, dbc, false, god, copts);

fail:
	free(file_guard);
	free(god);
	return rv;
}

