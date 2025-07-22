/**************************************************************************/
/*  tweak.h                                                               */
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

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"

class Tweak;
class PropertyTweaker;
class ObjectTweaker;
class TweakMonitor;

class TweakImpl
{
protected:
	int priority;
	int order;

	PropertyTweaker* p_owner;
	Variant tweak_value;
	TweakMonitor* pMonitor = nullptr;
public:
	virtual Variant apply(const Variant& value) const;

	_FORCE_INLINE_ int get_order() const { return order; }
	_FORCE_INLINE_ int get_priority() const { return priority; }
	_FORCE_INLINE_ const Variant& get_value() const { return tweak_value; }

	void set_owning_tweaker(PropertyTweaker* p_tweaker);
	void set_order(int order);
	void set_value(const Variant& val);
	void set_monitor(TweakMonitor* pM);
	void set_priority(int p);

	friend class Tweak;

	void disconnect();
	virtual ~TweakImpl();
};

class TweakAdd : public TweakImpl
{
public:
	virtual Variant apply(const Variant& value) const;
};

class TweakSubtract : public TweakImpl
{
public:
	virtual Variant apply(const Variant& value) const;
};

class TweakMultiply : public TweakImpl
{
public:
	virtual Variant apply(const Variant& value) const;
};

class TweakDivide : public TweakImpl
{
public:
	virtual Variant apply(const Variant& value) const;
};

class TweakLogicalAnd : public TweakImpl
{
public:
	virtual Variant apply(const Variant& value) const;
};

class TweakLogicalOr : public TweakImpl
{
public:
	virtual Variant apply(const Variant& value) const;
};


class TweakMonitor : public TweakImpl
{
	TweakImpl* pObserver;
public:
	virtual Variant apply(const Variant& value) const;
	virtual ~TweakMonitor();

	void set_observer(TweakImpl* pTweak);
};

class PropertyTweaker
{
	Variant base;
	Object* p_owner;
	StringName p_prop;
	List<TweakImpl*> tweaks;
	int tweak_order = 0;
public:
	Variant evaluate();
	Variant set_base(const Variant& p_value);

	void recalculate();

	void add_tweak(TweakImpl* p_tweak);
	void remove_tweak(TweakImpl* p_tweak);

	void remove_owner();

	Variant get_tweaked(const Variant& add_to_base);

	PropertyTweaker(Object* p_object, const StringName& p_property);
};

class ObjectTweaker
{
	Object* p_owner;
	HashMap<StringName, PropertyTweaker> props;
public:
	Variant set_base(const StringName &p_name, const Variant &p_value);

	PropertyTweaker* get_property_tweaker(const StringName& property);
	void set_owning_object(Object* p_object);
	Variant get_tweaked(const StringName& property, const Variant& add_to_base);

	~ObjectTweaker();
};


class Tweak : public RefCounted {
	GDCLASS(Tweak, RefCounted);
private:
	bool valid = false;
	TweakImpl* pImpl = nullptr;
public:
enum ActionType {
	ACTION_SET,
	ACTION_ADD,
	ACTION_SUBTRACT,
	ACTION_MULTIPLY,
	ACTION_DIVIDE,
	ACTION_AND,
	ACTION_OR
};

	Tweak();
	Tweak(Object* p_object, const StringName& p_property, const Variant &p_value, Tweak::ActionType action, int priority);
	Tweak(Object* p_object, const StringName& p_property, Object* p_source, const StringName& p_source_property, Tweak::ActionType action, int priority);

	void set_owning_tweaker(PropertyTweaker* p_tweaker);
	void set_order(int order);

	_FORCE_INLINE_ const Variant& get_value() const { return pImpl->get_value(); };
	void set_value(const Variant& value);
	
	_FORCE_INLINE_ int get_order() const { return pImpl->order; }
	_FORCE_INLINE_ int get_priority() const { return pImpl->priority; }

	~Tweak();

	static void _bind_methods();
};

VARIANT_ENUM_CAST(Tweak::ActionType);