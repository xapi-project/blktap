import unittest
import testlib
import os

import refcounter


class TestRefCounter(unittest.TestCase):
    @testlib.with_context
    def test_get_whencalled_creates_namespace(self, context):
        os.makedirs(refcounter.RefCounter.BASE_DIR)

        refcounter.RefCounter.get('not-important', False, 'somenamespace')

        self.assertEquals(
            ['somenamespace'],
            os.listdir(os.path.join(refcounter.RefCounter.BASE_DIR)))

    @testlib.with_context
    def test_get_whencalled_returns_counters(self, context):
        os.makedirs(refcounter.RefCounter.BASE_DIR)

        result = refcounter.RefCounter.get('not-important', False, 'somenamespace')

        self.assertEquals(1, result)

    @testlib.with_context
    def test_get_whencalled_creates_refcounter_file(self, context):
        os.makedirs(refcounter.RefCounter.BASE_DIR)

        refcounter.RefCounter.get('someobject', False, 'somenamespace')

        self.assertEquals(
            ['someobject'],
            os.listdir(os.path.join(
                refcounter.RefCounter.BASE_DIR, 'somenamespace')))

    @testlib.with_context
    def test_get_whencalled_refcounter_file_contents(self, context):
        os.makedirs(refcounter.RefCounter.BASE_DIR)

        refcounter.RefCounter.get('someobject', False, 'somenamespace')

        path_to_refcounter = os.path.join(
            refcounter.RefCounter.BASE_DIR, 'somenamespace', 'someobject')

        refcounter_file = open(path_to_refcounter, 'r')
        contents = refcounter_file.read()
        refcounter_file.close()

        self.assertEquals('1 0\n', contents)

    @testlib.with_context
    def test_put_is_noop_if_already_zero(self, context):
        os.makedirs(refcounter.RefCounter.BASE_DIR)

        result = refcounter.RefCounter.put(
            'someobject', False, 'somenamespace')

        self.assertEquals(0, result)
