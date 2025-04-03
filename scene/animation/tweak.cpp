/**************************************************************************/
/*  tweak.cpp                                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tweak.h"

#include "scene/main/node.h"
#include "core/variant/variant.h"
#include "core/object/object.h"

Tweak::Tweak() {
	ERR_FAIL_MSG("Tweak can't be created directly. Use create_tweak() method.");
}

Tweak::Tweak(Object* p_object, const StringName& p_property, const Variant &p_value, Tweak::ActionType action, int _priority) {
	if(p_object == nullptr)
	{
		return;
	}

	ObjectTweaker* p_obj_tweaker = p_object->get_object_tweaker();
	if(p_obj_tweaker == nullptr)
	{
		p_obj_tweaker = memnew(ObjectTweaker);
		p_object->set_object_tweaker(p_obj_tweaker);
	}

	PropertyTweaker* p_prop_tweaker = p_obj_tweaker->get_property_tweaker(p_property);
	if(p_prop_tweaker == nullptr)
	{
		return;
	}

	switch (action)
	{
	case ACTION_ADD:  pImpl = memnew(TweakAdd); break;
	case ACTION_SUBTRACT:  pImpl = memnew(TweakSubtract); break;
	case ACTION_MULTIPLY:  pImpl = memnew(TweakMultiply); break;
	case ACTION_DIVIDE:  pImpl = memnew(TweakDivide); break;
	case ACTION_SET:
	default: pImpl = memnew(TweakImpl); break;
	}

	pImpl->priority = _priority;
	pImpl->tweak_value = p_value;

	p_prop_tweaker->add_tweak(pImpl);

	valid = true;
}

void Tweak::set_owning_tweaker(PropertyTweaker *p_tweaker) {
	pImpl->p_owner = p_tweaker;
}

void Tweak::set_order(int _order) {
	pImpl->order = _order;
}

Tweak::~Tweak() {
	if(pImpl)
	{
		memdelete(pImpl);
	}
}

void Tweak::_bind_methods() {
	BIND_ENUM_CONSTANT(ACTION_ADD);
	BIND_ENUM_CONSTANT(ACTION_SUBTRACT);
	BIND_ENUM_CONSTANT(ACTION_MULTIPLY);
	BIND_ENUM_CONSTANT(ACTION_DIVIDE);
	BIND_ENUM_CONSTANT(ACTION_SET);
}

Variant PropertyTweaker::evaluate() {
	Variant value = base;
	for(TweakImpl* p_tweak : tweaks)
	{
		value = p_tweak->apply(value);
	}
	return value;
}

Variant PropertyTweaker::set_base(const Variant &p_value) {
	base = p_value;
	return evaluate();
}

class TweakSort
{
public:
	bool operator()(TweakImpl* a, TweakImpl* b)
	{
		if(a->get_priority() < b->get_priority())
		{
			return true;
		}
		if(a->get_priority() > b->get_priority())
		{
			return false;
		}
		return a->get_order() < b->get_order();
	}
};

void PropertyTweaker::recalculate() {
	tweaks.sort_custom_inplace<TweakSort>();
	Variant val = evaluate();
	p_owner->set_direct(p_prop, val);
}

Variant ObjectTweaker::set_base(const StringName &p_name, const Variant &p_value) {
	PropertyTweaker* ptr = props.getptr(p_name);
	if(ptr == nullptr)
	{
		return p_value;
	}
	return ptr->set_base(p_value);
}

PropertyTweaker *ObjectTweaker::get_property_tweaker(const StringName &property) {
	PropertyTweaker* p_tweaker = props.getptr(property);
	if(p_tweaker!=nullptr)
	{
		return p_tweaker;
	}
	props.insert(property, PropertyTweaker(p_owner, property));
	return props.getptr(property);
}

void ObjectTweaker::set_owning_object(Object *p_object) {
	p_owner = p_object;
}

void PropertyTweaker::add_tweak(TweakImpl *p_tweak) {
	p_tweak->set_owning_tweaker(this);
	p_tweak->set_order(tweak_order++);
	tweaks.push_back(p_tweak);
	recalculate();
}

void PropertyTweaker::remove_tweak(TweakImpl *p_tweak) {
	tweaks.erase(p_tweak);
	p_tweak->set_owning_tweaker(nullptr);
	recalculate();
}

PropertyTweaker::PropertyTweaker(Object *p_obj, const StringName &p_property) : p_owner(p_obj), p_prop(p_property)  {
	base = p_owner->get(p_prop);
}

Variant TweakImpl::apply(const Variant &value) const {
	return tweak_value;
}

void TweakImpl::set_owning_tweaker(PropertyTweaker *p_tweaker) {
	p_owner = p_tweaker;
}

void TweakImpl::set_order(int _order) {
	order = _order;
}

TweakImpl::~TweakImpl() {
	if(p_owner)
	{
		p_owner->remove_tweak(this);
	}
}

Variant TweakAdd::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_ADD, value, tweak_value);
}

Variant TweakSubtract::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_SUBTRACT, value, tweak_value);
}

Variant TweakMultiply::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_MULTIPLY, value, tweak_value);
}

Variant TweakDivide::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_DIVIDE, value, tweak_value);
}
