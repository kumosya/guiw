#include "number.h"

#include <cstring>
#include <vector>
namespace calc {
namespace {
constexpr size_t maximumDigits = 4096;
struct Context {
    uint64_t work = 0;
    const char* error = nullptr;
    const uint32_t* generation;
    uint32_t expected;
    bool spend(uint64_t cost = 1) {
        work += cost;
        if (generation && __atomic_load_n(generation, __ATOMIC_ACQUIRE) != expected)
            error = "Cancelled";
        if (work > 25000000) error = "Work limit exceeded";
        return !error;
    }
    void fail(const char* message) {
        if (!error) error = message;
    }
};
struct Integer {
    std::vector<uint8_t> digits;
    Integer(uint32_t value = 0) {
        do {
            digits.push_back(value % 10);
            value /= 10;
        } while (value);
    }
    bool zero() const { return digits.size() == 1 && !digits[0]; }
    void trim() {
        while (digits.size() > 1 && !digits.back()) digits.pop_back();
    }
};
bool resize(Integer& value, size_t size, Context& context) {
    if (size > maximumDigits) {
        context.fail("Digit limit exceeded");
        return false;
    }
    value.digits.resize(size);
    if (value.digits.size() != size) {
        context.fail("Out of memory");
        return false;
    }
    return true;
}
void shift(Integer& value, size_t count, Context& context) {
    if (!count || value.zero() || context.error) return;
    const size_t old = value.digits.size();
    if (!resize(value, old + count, context) || !context.spend(old + count)) return;
    for (size_t i = old; i; --i) value.digits[i - 1 + count] = value.digits[i - 1];
    for (size_t i = 0; i < count; ++i) value.digits[i] = 0;
}
int compare(const Integer& a, const Integer& b) {
    if (a.digits.size() != b.digits.size())
        return a.digits.size() > b.digits.size() ? 1 : -1;
    for (size_t i = a.digits.size(); i; --i)
        if (a.digits[i - 1] != b.digits[i - 1])
            return a.digits[i - 1] > b.digits[i - 1] ? 1 : -1;
    return 0;
}
Integer add(const Integer& a, const Integer& b, Context& context) {
    Integer out;
    const size_t count =
        a.digits.size() > b.digits.size() ? a.digits.size() : b.digits.size();
    if (!resize(out, count, context) || !context.spend(count)) return out;
    unsigned carry = 0;
    for (size_t i = 0; i < count; ++i) {
        unsigned sum = carry + (i < a.digits.size() ? a.digits[i] : 0) +
                       (i < b.digits.size() ? b.digits[i] : 0);
        out.digits[i] = sum % 10;
        carry = sum / 10;
    }
    if (carry) {
        if (!resize(out, count + 1, context)) return out;
        out.digits[count] = carry;
    }
    return out;
}
Integer subtract(const Integer& a, const Integer& b, Context& context) {
    Integer out = a;
    if (!context.spend(a.digits.size())) return out;
    int borrow = 0;
    for (size_t i = 0; i < out.digits.size(); ++i) {
        int digit = int(a.digits[i]) - (i < b.digits.size() ? b.digits[i] : 0) - borrow;
        borrow = digit < 0;
        out.digits[i] = digit + (borrow ? 10 : 0);
    }
    out.trim();
    return out;
}
Integer multiply(const Integer& a, const Integer& b, Context& context) {
    Integer out;
    if (a.zero() || b.zero()) return out;
    if (!context.spend(a.digits.size() * b.digits.size()) ||
        !resize(out, a.digits.size() + b.digits.size(), context))
        return out;
    for (size_t i = 0; i < a.digits.size(); ++i) {
        unsigned carry = 0;
        for (size_t j = 0; j < b.digits.size(); ++j) {
            unsigned value = out.digits[i + j] + a.digits[i] * b.digits[j] + carry;
            out.digits[i + j] = value % 10;
            carry = value / 10;
        }
        out.digits[i + b.digits.size()] = carry;
    }
    out.trim();
    return out;
}
void divmod(const Integer& a, const Integer& b, Integer& quotient, Integer& remainder,
            Context& context) {
    if (b.zero()) {
        context.fail("Division by zero");
        return;
    }
    if (!resize(quotient, a.digits.size(), context)) return;
    remainder = Integer();
    for (size_t i = a.digits.size(); i && !context.error; --i) {
        shift(remainder, 1, context);
        remainder.digits[0] = a.digits[i - 1];
        remainder.trim();
        uint8_t digit = 0;
        while (compare(remainder, b) >= 0 && context.spend(b.digits.size())) {
            remainder = subtract(remainder, b, context);
            ++digit;
        }
        quotient.digits[i - 1] = digit;
    }
    quotient.trim();
}
struct Decimal {
    Integer coefficient;
    int scale = 0;
    bool negative = false, approximate = false;
    void normalize() {
        if (coefficient.zero()) {
            scale = 0;
            negative = false;
            return;
        }
        size_t remove = 0;
        while (remove < size_t(scale) && remove < coefficient.digits.size() - 1 &&
               coefficient.digits[remove] == 0)
            ++remove;
        if (remove) {
            for (size_t i = remove; i < coefficient.digits.size(); ++i)
                coefficient.digits[i - remove] = coefficient.digits[i];
            coefficient.digits.resize(coefficient.digits.size() - remove);
            scale -= int(remove);
        }
    }
};
void align(Decimal& a, Decimal& b, Context& context) {
    const int scale = a.scale > b.scale ? a.scale : b.scale;
    shift(a.coefficient, scale - a.scale, context);
    shift(b.coefficient, scale - b.scale, context);
    a.scale = b.scale = scale;
}
Decimal sum(Decimal a, Decimal b, Context& context) {
    align(a, b, context);
    Decimal out;
    out.scale = a.scale;
    out.approximate = a.approximate || b.approximate;
    if (a.negative == b.negative) {
        out.coefficient = add(a.coefficient, b.coefficient, context);
        out.negative = a.negative;
    } else if (compare(a.coefficient, b.coefficient) >= 0) {
        out.coefficient = subtract(a.coefficient, b.coefficient, context);
        out.negative = a.negative;
    } else {
        out.coefficient = subtract(b.coefficient, a.coefficient, context);
        out.negative = b.negative;
    }
    out.normalize();
    return out;
}
Decimal product(const Decimal& a, const Decimal& b, Context& context) {
    Decimal out;
    out.coefficient = multiply(a.coefficient, b.coefficient, context);
    out.scale = a.scale + b.scale;
    if (out.scale > int(maximumDigits)) context.fail("Scale limit exceeded");
    out.negative = a.negative != b.negative;
    out.approximate = a.approximate || b.approximate;
    out.normalize();
    return out;
}
Decimal divide(const Decimal& a, const Decimal& b, uint32_t precision,
               Context& context) {
    Decimal out;
    if (b.coefficient.zero()) {
        context.fail("Division by zero");
        return out;
    }
    if (a.coefficient.zero()) return out;
    Integer left = a.coefficient, right = b.coefficient;
    const int difference = int(left.digits.size()) - int(right.digits.size());
    int exponent = difference + b.scale - a.scale;
    if (difference > 0)
        shift(right, difference, context);
    else
        shift(left, -difference, context);
    if (compare(left, right) < 0) --exponent;
    out.scale = int(precision) - 1 - exponent;
    const int power = b.scale - a.scale + out.scale;
    left = a.coefficient;
    right = b.coefficient;
    if (power > 0)
        shift(left, power, context);
    else
        shift(right, -power, context);
    Integer remainder;
    divmod(left, right, out.coefficient, remainder, context);
    out.approximate = a.approximate || b.approximate || !remainder.zero();
    Integer doubled = add(remainder, remainder, context);
    const int half = compare(doubled, right);
    if (half > 0 || (half == 0 && (out.coefficient.digits[0] & 1)))
        out.coefficient = add(out.coefficient, Integer(1), context);
    if (out.scale < 0) {
        shift(out.coefficient, -out.scale, context);
        out.scale = 0;
    }
    out.negative = a.negative != b.negative;
    out.normalize();
    return out;
}
Decimal modulo(Decimal a, Decimal b, Context& context) {
    if (b.coefficient.zero()) {
        context.fail("Modulo by zero");
        return {};
    }
    align(a, b, context);
    Integer quotient, remainder;
    divmod(a.coefficient, b.coefficient, quotient, remainder, context);
    a.coefficient = remainder;
    a.approximate = a.approximate || b.approximate;
    a.normalize();
    return a;
}
Decimal power(const Decimal& base, const Decimal& exponent, uint32_t precision,
              Context& context) {
    if (exponent.scale) {
        context.fail("Non-integer exponent is unsupported");
        return {};
    }
    unsigned value = 0;
    for (size_t i = exponent.coefficient.digits.size(); i; --i) {
        if (value > 4096 / 10) {
            context.fail("Exponent limit exceeded");
            return {};
        }
        value = value * 10 + exponent.coefficient.digits[i - 1];
    }
    if (value > 4096) {
        context.fail("Exponent limit exceeded");
        return {};
    }
    if (base.coefficient.zero() && (!value || exponent.negative)) {
        context.fail(value ? "Zero to negative power" : "Zero to zero power");
        return {};
    }
    Decimal out;
    out.coefficient = Integer(1);
    Decimal factor = base;
    while (value && context.spend()) {
        if (value & 1) out = product(out, factor, context);
        value >>= 1;
        if (value) factor = product(factor, factor, context);
    }
    if (exponent.negative) {
        Decimal one;
        one.coefficient = Integer(1);
        out = divide(one, out, precision, context);
    }
    out.approximate = out.approximate || exponent.approximate;
    return out;
}
class Parser {
    const char* text_;
    size_t offset_ = 0;
    unsigned depth_ = 0;
    uint32_t precision_;
    Context& context_;
    void spaces() {
        while (text_[offset_] == ' ' || text_[offset_] == '\t') ++offset_;
    }
    bool take(char token) {
        spaces();
        if (text_[offset_] != token) return false;
        ++offset_;
        return true;
    }
    Decimal primary() {
        if (++depth_ > 32 || !context_.spend()) {
            context_.fail("Expression nesting limit exceeded");
            --depth_;
            return {};
        }
        Decimal out;
        if (take('(')) {
            out = expression();
            if (!take(')')) context_.fail("Expected closing parenthesis");
        } else {
            spaces();
            size_t start = offset_;
            bool decimal = false, hasDigit = false;
            while ((text_[offset_] >= '0' && text_[offset_] <= '9') ||
                   text_[offset_] == '.') {
                char c = text_[offset_++];
                if (c == '.') {
                    if (decimal) {
                        context_.fail("Invalid decimal");
                        break;
                    }
                    decimal = true;
                    continue;
                }
                hasDigit = true;
                shift(out.coefficient, 1, context_);
                out.coefficient.digits[0] = c - '0';
                if (decimal) ++out.scale;
            }
            if (!hasDigit || offset_ == start) context_.fail("Expected a number");
            out.normalize();
        }
        --depth_;
        return out;
    }
    Decimal exponent() {
        Decimal left = primary();
        if (take('^')) left = power(left, unary(), precision_, context_);
        return left;
    }
    Decimal unary() {
        if (++depth_ > 32) {
            context_.fail("Expression nesting limit exceeded");
            --depth_;
            return {};
        }
        Decimal out;
        if (take('-')) {
            out = unary();
            if (!out.coefficient.zero()) out.negative = !out.negative;
        } else if (take('+'))
            out = unary();
        else
            out = exponent();
        --depth_;
        return out;
    }
    Decimal term() {
        Decimal left = unary();
        while (!context_.error) {
            if (take('*'))
                left = product(left, unary(), context_);
            else if (take('/'))
                left = divide(left, unary(), precision_, context_);
            else if (take('%'))
                left = modulo(left, unary(), context_);
            else
                break;
        }
        return left;
    }
    Decimal expression() {
        Decimal left = term();
        while (!context_.error) {
            if (take('+'))
                left = sum(left, term(), context_);
            else if (take('-')) {
                Decimal right = term();
                right.negative = !right.negative;
                left = sum(left, right, context_);
            } else
                break;
        }
        return left;
    }

   public:
    Parser(const char* text, uint32_t precision, Context& context)
        : text_(text), precision_(precision), context_(context) {}
    Decimal run() {
        Decimal out = expression();
        spaces();
        if (text_[offset_]) context_.fail("Unexpected token");
        return out;
    }
};
std::string format(const Decimal& value) {
    char text[maximumDigits * 2 + 4];
    size_t length = 0;
    if (value.negative) text[length++] = '-';
    const int digits = int(value.coefficient.digits.size());
    if (value.scale >= digits) {
        text[length++] = '0';
        text[length++] = '.';
        for (int i = 0; i < value.scale - digits; ++i) text[length++] = '0';
    }
    for (int i = digits; i; --i) {
        text[length++] = char('0' + value.coefficient.digits[i - 1]);
        if (i - 1 == value.scale && value.scale) text[length++] = '.';
    }
    text[length] = 0;
    return std::string(text);
}
}  // namespace
Result evaluate(const char* expression, uint32_t precision, const uint32_t* generation,
                uint32_t expected) {
    Context context{0, nullptr, generation, expected};
    if (!expression || strlen(expression) > 2048 || precision < 1 || precision > 1000)
        return {"", false, "Input or precision limit exceeded"};
    Decimal value = Parser(expression, precision, context).run();
    if (context.error) return {"", false, context.error};
    return {format(value), value.approximate, nullptr};
}
}  // namespace calc
