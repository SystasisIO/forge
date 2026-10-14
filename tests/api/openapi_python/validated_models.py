"""Schema-first consumption of untouched generated Pydantic models."""

from decimal import Decimal, InvalidOperation
import json

from jsonschema import Draft202012Validator, validators


def load_json(text):
    """Parse exact numbers, rejecting non-JSON constants and duplicate keys."""
    def object_members(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise json.JSONDecodeError("duplicate object member", text, 0)
            value[key] = item
        return value

    def invalid_constant(_):
        raise json.JSONDecodeError("non-finite JSON number", text, 0)

    try:
        return json.loads(text, parse_float=Decimal, parse_constant=invalid_constant,
                          object_pairs_hook=object_members)
    except json.JSONDecodeError:
        raise
    except (ValueError, InvalidOperation) as error:
        raise json.JSONDecodeError("unrepresentable JSON number", text, 0) from error


def _is_integer(_checker, value):
    # JSON Schema integers are mathematical integers, including decimal tokens.
    return (Draft202012Validator.TYPE_CHECKER.is_type(value, "integer")
            or (isinstance(value, Decimal) and value.is_finite()
                and value == value.to_integral_value()))


_validator = validators.extend(
    Draft202012Validator,
    type_checker=Draft202012Validator.TYPE_CHECKER.redefine("integer", _is_integer),
)


def schema_validator(document, schema_name):
    schema = document["components"]["schemas"][schema_name]
    _validator.check_schema(schema)
    pointer_name = schema_name.replace("~", "~0").replace("/", "~1")
    return _validator({
        "$ref": f"#/components/schemas/{pointer_name}",
        "components": document["components"],
    })


class ValidatedModels:
    """Validate raw JSON by schema before Pydantic can normalize its values."""

    def __init__(self, document, models):
        self._models = models
        self._validators = {
            name: schema_validator(document, name)
            for name in document["components"]["schemas"]
        }

    def validate_json(self, schema_name, text):
        self._validators[schema_name].validate(load_json(text))
        # Do not roundtrip through Python containers: strict JSON tuple handling
        # and the generator's numeric lexical limitations must remain visible.
        return getattr(self._models, schema_name).model_validate_json(text, strict=True)
