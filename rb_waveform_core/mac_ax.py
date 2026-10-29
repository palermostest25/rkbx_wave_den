"""Read-only macOS Accessibility bindings. No input events or AX setters."""
from __future__ import annotations
import ctypes as C
from dataclasses import dataclass
import sys
import time


@dataclass
class AXText:
    element: int
    role: str
    value: str
    title: str
    help: str

    @property
    def text(self):
        return self.value or self.title


class Accessibility:
    def __init__(self):
        if sys.platform != 'darwin':
            raise OSError('macOS Accessibility requires macOS')
        self.ax = C.CDLL('/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices')
        self.cf = C.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
        def bind(lib, name, result, *arguments):
            f=getattr(lib,name); f.restype=result; f.argtypes=list(arguments); return f
        ptr=C.c_void_p; index=C.c_long
        self.trusted=bind(self.ax,'AXIsProcessTrusted',C.c_bool)
        self.application=bind(self.ax,'AXUIElementCreateApplication',ptr,C.c_int)
        self.copy=bind(self.ax,'AXUIElementCopyAttributeValue',C.c_int,ptr,ptr,C.POINTER(ptr))
        self.timeout=bind(self.ax,'AXUIElementSetMessagingTimeout',C.c_int,ptr,C.c_float)
        self.release=bind(self.cf,'CFRelease',None,ptr)
        self.retain=bind(self.cf,'CFRetain',ptr,ptr)
        self.string=bind(self.cf,'CFStringCreateWithCString',ptr,ptr,C.c_char_p,C.c_uint32)
        self.length=bind(self.cf,'CFStringGetLength',index,ptr)
        self.get_string=bind(self.cf,'CFStringGetCString',C.c_bool,ptr,C.c_void_p,index,C.c_uint32)
        self.type_id=bind(self.cf,'CFGetTypeID',C.c_ulong,ptr)
        self.string_id=bind(self.cf,'CFStringGetTypeID',C.c_ulong)
        self.array_id=bind(self.cf,'CFArrayGetTypeID',C.c_ulong)
        self.count=bind(self.cf,'CFArrayGetCount',index,ptr)
        self.item=bind(self.cf,'CFArrayGetValueAtIndex',ptr,ptr,index)
        self.attributes={}
        self.elements=[]

    def request_permission(self):
        ptr=C.c_void_p
        create=self.cf.CFDictionaryCreate
        create.restype=ptr
        create.argtypes=[ptr,C.POINTER(ptr),C.POINTER(ptr),C.c_long,ptr,ptr]
        key=C.c_void_p.in_dll(self.ax,'kAXTrustedCheckOptionPrompt').value
        value=C.c_void_p.in_dll(self.cf,'kCFBooleanTrue').value
        options=create(None,(ptr*1)(key),(ptr*1)(value),1,None,None)
        request=self.ax.AXIsProcessTrustedWithOptions
        request.argtypes=[ptr];request.restype=C.c_bool
        try:return request(options)
        finally:self.release(options)

    def _attribute(self, element, name):
        if name not in self.attributes:
            self.attributes[name]=self.string(None,name.encode(),0x08000100)
        result=C.c_void_p()
        error = self.copy(element,self.attributes[name],C.byref(result))
        if error == -25204:
            raise OSError('Rekordbox is not responding to Accessibility requests')
        if error:
            return None
        return result.value

    def text(self, element, name='AXValue'):
        value=self._attribute(element,name)
        if not value:
            return ''
        try:
            if self.type_id(value)!=self.string_id():
                return ''
            size=self.length(value)*4+1
            buffer=C.create_string_buffer(size)
            return buffer.value.decode('utf-8') if self.get_string(value,buffer,size,0x08000100) else ''
        finally:
            self.release(value)

    def snapshot(self, pid, *, limit=2000):
        if not self.trusted():
            raise PermissionError('Allow Rekordbox Wave in System Settings → Privacy & Security → Accessibility')
        self.clear_elements()
        app=self.application(pid)
        if not app:
            raise OSError('Rekordbox is no longer running')
        self.timeout(app,.1)
        nodes=[]
        deadline=time.monotonic()+2
        def visit(element, depth):
            if time.monotonic()>deadline or len(nodes)>=limit:
                raise OSError('Rekordbox Accessibility window is too large to scan promptly')
            if depth>16:
                return
            role=self.text(element,'AXRole')
            if role in ('AXMenuBar','AXMenu','AXMenuItem','AXMenuBarItem'):
                return
            self.elements.append(self.retain(element))
            nodes.append(AXText(element,role,self.text(element),
                                self.text(element,'AXTitle'),self.text(element,'AXHelp')))
            children=self._attribute(element,'AXChildren')
            if children:
                try:
                    if self.type_id(children)==self.array_id():
                        for i in range(self.count(children)):
                            visit(self.item(children,i),depth+1)
                finally:
                    self.release(children)
        try:
            visit(app,0)
        finally:
            self.release(app)
        return nodes

    def clear_elements(self):
        for element in self.elements:
            self.release(element)
        self.elements.clear()

    def close(self):
        self.clear_elements()
        for attribute in self.attributes.values():
            self.release(attribute)
        self.attributes.clear()
