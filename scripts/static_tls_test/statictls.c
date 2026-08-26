__thread unsigned char bigtls[20000000] __attribute__((tls_model("initial-exec")));

void set_bigtls()
{
   for (unsigned i = 0; i < sizeof(bigtls); i++) {
      bigtls[i] = (unsigned char) i % 255;
   }
}

int get_bigtls()
{
   int result = 0;
   for (unsigned i = 0; i < sizeof(bigtls); i++) {
      result += bigtls[i];
   }
   return result;
}
