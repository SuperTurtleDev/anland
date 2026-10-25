#!/usr/bin/env python3
"""Run the production recognizer with minimal Android stubs, no device required.

The stubs model contacts and coordinate transforms only; device/evdev delivery
and native desktop integration still require a real tablet.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/Touchpad.java'
STUBS = {
'android/content/Context.java': 'package android.content; public class Context {}',
'android/util/Log.java': 'package android.util; public class Log { public static int d(String t,String s){return 0;} }',
'android/view/InputDevice.java': 'package android.view; public class InputDevice { public static final int SOURCE_TOUCHSCREEN=4098; }',
'android/view/ViewConfiguration.java': '''package android.view;
public class ViewConfiguration {
 public static ViewConfiguration get(android.content.Context c){return new ViewConfiguration();}
 public int getScaledTouchSlop(){return 8;}
}''',
'android/graphics/Matrix.java': '''package android.graphics;
public class Matrix {
 public float sx=1,sy=1,tx,ty;
 public void setScale(float x,float y){sx=x;sy=y;tx=ty=0;}
 public void postTranslate(float x,float y){tx+=x;ty+=y;}
}''',
'android/view/MotionEvent.java': '''package android.view;
public class MotionEvent {
 public static final int ACTION_DOWN=0,ACTION_UP=1,ACTION_MOVE=2,ACTION_CANCEL=3,
 ACTION_POINTER_DOWN=5,ACTION_POINTER_UP=6;
 private final int action,index; private final long time; private final float[] xy;
 public MotionEvent(int a,int i,long t,float... xy){action=a;index=i;time=t;this.xy=xy.clone();}
 public static MotionEvent obtain(MotionEvent e){return new MotionEvent(e.action,e.index,e.time,e.xy);}
 public int getActionMasked(){return action;} public int getActionIndex(){return index;}
 public long getEventTime(){return time;} public int getPointerCount(){return xy.length/2;}
 public int getPointerId(int i){return i;}
 public float getX(){return getX(0);} public float getY(){return getY(0);}
 public float getX(int i){return xy[i*2];} public float getY(int i){return xy[i*2+1];}
 public void setSource(int s){} public void recycle(){}
 public void transform(android.graphics.Matrix m){for(int i=0;i<xy.length;i+=2){xy[i]=xy[i]*m.sx+m.tx;xy[i+1]=xy[i+1]*m.sy+m.ty;}}
}''',
'com/anland/consumer/TouchpadTest.java': '''package com.anland.consumer;
import android.view.MotionEvent;
import static android.view.MotionEvent.*;
public class TouchpadTest implements Touchpad.Output {
 int buttons,scrolls,touches; Touchpad pad;
 TouchpadTest(boolean tap,int guard){pad=new Touchpad(new android.content.Context(),this,true);pad.setTapFilter(tap,guard,false);}
 void e(int a,int i,long t,float... xy){pad.onTouch(new MotionEvent(a,i,t,xy));}
 void tap(long t){e(ACTION_DOWN,0,t,10,10);e(ACTION_UP,0,t+20,10,10);}
 void scroll(long t, boolean firstLifts){
  e(ACTION_DOWN,0,t,10,10);e(ACTION_POINTER_DOWN,1,t+10,10,10,30,10);
  e(ACTION_MOVE,0,t+20,10,30,30,30);
  e(ACTION_POINTER_UP,firstLifts?0:1,t+30,10,30,30,30);
  e(ACTION_UP,0,t+40,firstLifts?30:10,30);
 }
 void check(boolean ok,String name){if(!ok)throw new AssertionError(name);}
 public void onMotion(float x,float y){} public void onScroll(int a,float v){scrolls++;}
 public void onButton(int b,boolean p){buttons++;}
 public void onTouch(int a,int i,float x,float y){touches++;} public void onTouchFrame(){}
 public float cursorX(){return 100;} public float cursorY(){return 100;}
 public static void main(String[] args){
  TouchpadTest x=new TouchpadTest(false,300);
  x.tap(1000);x.e(ACTION_DOWN,0,1100,10,10);x.e(ACTION_MOVE,0,1700,10,10);x.e(ACTION_UP,0,1720,10,10);
  x.e(ACTION_DOWN,0,2000,10,10);x.e(ACTION_POINTER_DOWN,1,2010,10,10,30,10);x.e(ACTION_POINTER_UP,1,2020,10,10,30,10);x.e(ACTION_UP,0,2030,10,10);
  x.check(x.buttons==0,"disabled: single tap, two-finger tap and long-press emit no clicks");
  x.scroll(3000,true);x.check(x.scrolls>0 && x.buttons==0,"disabled taps retain scroll");
  for(boolean first:new boolean[]{false,true}){
   x=new TouchpadTest(true,300);x.scroll(1000,first);
   x.check(x.scrolls>0 && x.buttons==0,"scroll + either lift order never clicks");
   x.tap(1100);x.check(x.buttons==0,"post-scroll recontact suppressed");
   x.tap(1500);x.check(x.buttons==2,"deliberate tap after guard works");
  }
  x=new TouchpadTest(true,0);x.scroll(1000,false);x.tap(1100);
  x.check(x.buttons==2,"zero guard allows next tap, scroll itself still never clicks");
  x=new TouchpadTest(true,300);x.e(ACTION_DOWN,0,1000,10,10);x.e(ACTION_POINTER_DOWN,1,1010,10,10,30,10);
  x.e(ACTION_MOVE,0,1020,10,30,30,30);x.e(ACTION_POINTER_UP,1,1030,10,30,30,30);
  x.e(ACTION_POINTER_DOWN,1,1040,10,30,30,30);x.e(ACTION_POINTER_UP,1,1050,10,30,30,30);x.e(ACTION_UP,0,1060,10,30);
  x.check(x.buttons==0,"finger rejoins a scroll: cannot rearm right click");
  x=new TouchpadTest(true,300);x.tap(1000);x.check(x.buttons==2,"enabled single tap works");
  x=new TouchpadTest(true,300);x.e(ACTION_DOWN,0,1000,10,10);x.e(ACTION_POINTER_DOWN,1,1010,10,10,30,10);x.e(ACTION_POINTER_UP,1,1020,10,10,30,10);x.e(ACTION_UP,0,1030,10,10);
  x.check(x.buttons==2,"enabled deliberate two-finger tap works");
  x=new TouchpadTest(true,300);x.e(ACTION_DOWN,0,1000,10,10);x.e(ACTION_MOVE,0,1600,10,10);x.pad.cancel();
  x.check(x.buttons==2,"cancel releases synthesized drag");
  // Regression: classification at a subpixel threshold used to leave right-tap armed.
  x=new TouchpadTest(true,300);x.pad.setGestureThresholds(.05f,2.35f);
  x.e(ACTION_DOWN,0,1000,10,10);x.e(ACTION_POINTER_DOWN,1,1010,10,10,30,10);x.e(ACTION_MOVE,0,1020,10,10.5f,30,10.5f);
  x.e(ACTION_POINTER_UP,1,1030,10,10.5f,30,10.5f);x.e(ACTION_UP,0,1040,10,10.5f);
  x.check(x.buttons==0,"classified subpixel scroll cannot become right click");
  // A real OnePlus trace: the leading contact moves before the other updates.
  x=new TouchpadTest(true,0);x.pad.setMultiFingerGesturesDisabled(true);
  x.e(ACTION_DOWN,0,1000,10,100);x.e(ACTION_POINTER_DOWN,1,1010,10,100,30,100);
  x.e(ACTION_MOVE,0,1050,10,100,30,70); // formerly permanently swallowed as pinch
  x.e(ACTION_MOVE,0,1060,10,65,30,60);
  x.e(ACTION_POINTER_UP,1,1070,10,65,30,60);x.e(ACTION_UP,0,1080,10,65);
  x.check(x.scrolls>0 && x.buttons==0,"lagging finger can still establish scroll");
  x=new TouchpadTest(true,0);x.pad.setMultiFingerGesturesDisabled(true);
  x.e(ACTION_DOWN,0,1000,10,100);x.e(ACTION_POINTER_DOWN,1,1010,10,100,30,100);
  x.e(ACTION_MOVE,0,1050,10,70,30,130); // actual opposite-motion pinch
  x.e(ACTION_POINTER_UP,1,1060,10,70,30,130);x.e(ACTION_UP,0,1070,10,70);
  x.check(x.scrolls==0 && x.buttons==0,"disabled pinch never falls through to right tap");
  x=new TouchpadTest(true,0);
  x.e(ACTION_DOWN,0,1000,10,10);x.e(ACTION_POINTER_DOWN,1,1010,10,10,30,10);
  x.e(ACTION_POINTER_UP,1,1400,10,10,30,10);x.e(ACTION_UP,0,1410,10,10);
  x.check(x.buttons==0,"lifting a finger must not reset the tap duration");
  x=new TouchpadTest(true,0);
  x.e(ACTION_DOWN,0,1000,10,10);x.e(ACTION_POINTER_DOWN,1,1010,10,10,30,10);
  x.e(ACTION_POINTER_UP,1,1050,10,30,30,30);x.e(ACTION_UP,0,1060,10,30);
  x.check(x.buttons==0,"movement arriving with release is not a tap");
  x=new TouchpadTest(true,0);x.scroll(1000,false);x.tap(1100);
  x.check(x.buttons==2,"default zero cooldown retains intentional post-scroll click");
  System.out.println("PASS: touchpad regression scenarios including lagging contacts and pinch");
 }
}'''
}
with tempfile.TemporaryDirectory(prefix='anland-touchpad-test-') as tmp:
    tmp=Path(tmp)
    for name,contents in STUBS.items():
        p=tmp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(contents)
    subprocess.run(['javac','-d',str(tmp),str(SOURCE),*[str(tmp/p) for p in STUBS]],check=True)
    subprocess.run(['java','-cp',str(tmp),'com.anland.consumer.TouchpadTest'],check=True)
